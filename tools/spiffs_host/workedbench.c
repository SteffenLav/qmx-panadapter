// What does "worked before" cost at boot once the archive is large?
//
// load_from_file() is O(records) parse + cache_add(), and cache_add() LINEARLY
// SCANS the unique (call, band) pairs already held - so the whole thing is
// O(records x pairs). Fine at 2,715 records; the question for the SD-rotation
// design (docs/qso-log-rotation-design.md) is what it becomes at 10,000+
// archived records, and whether a pre-deduplicated index on the card is worth
// the extra code.
//
// Two paths, same data:
//   option 1  scan on insert   - the code as it stands today
//   option 2  append, no scan  - what a pre-deduplicated index allows
//
// Host CPU only. See the note on DEV_MS_AT_2715 below for why there is no
// device column: the one device figure that exists is dominated by file I/O,
// so scaling CPU by it would be wrong.
//
//   gcc -O2 -o workedbench.exe workedbench.c && ./workedbench.exe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define ADIF_CALL_MAX 16
#define ADIF_BAND_MAX 6

/* ---- verbatim from main/adif/adif_log.c ---- */
typedef struct { uint32_t hash; char call[ADIF_CALL_MAX]; char band[ADIF_BAND_MAX]; } worked_entry_t;
static int g_nodedup;
static worked_entry_t *s_worked;
static int s_worked_count, s_worked_cap;

static uint32_t call_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static bool cache_grow(void)
{
    int want = s_worked_cap + 1024;
    worked_entry_t *p = realloc(s_worked, (size_t)want * sizeof(worked_entry_t));
    if (!p) return false;
    s_worked = p; s_worked_cap = want; return true;
}
static void cache_add(const char *call, const char *band)
{
    if (!call || !call[0]) return;
    if (!band) band = "";
    uint32_t h = call_hash(call);
    if (g_nodedup) {                       /* option 2: the index is already unique */
        if (s_worked_count == s_worked_cap && !cache_grow()) return;
        goto store;
    }
    for (int i = 0; i < s_worked_count; i++) {
        if (s_worked[i].hash != h) continue;
        if (strcmp(s_worked[i].call, call) == 0 &&
            strcmp(s_worked[i].band, band) == 0) return;
    }
    if (s_worked_count == s_worked_cap && !cache_grow()) return;
store:
    s_worked[s_worked_count].hash = h;
    strncpy(s_worked[s_worked_count].call, call, ADIF_CALL_MAX - 1);
    s_worked[s_worked_count].call[ADIF_CALL_MAX - 1] = '\0';
    strncpy(s_worked[s_worked_count].band, band, ADIF_BAND_MAX - 1);
    s_worked[s_worked_count].band[ADIF_BAND_MAX - 1] = '\0';
    s_worked_count++;
}
static bool extract_field(const char *line, const char *field, char *out, size_t out_sz)
{
    char tag[24]; int tl = snprintf(tag, sizeof(tag), "<%s:", field);
    const char *p = strstr(line, tag);
    if (!p) return false;
    p += tl; int len = atoi(p);
    const char *close = strchr(p, '>');
    if (!close || len <= 0 || (size_t)len >= out_sz) return false;
    int copy = (len < (int)out_sz - 1) ? len : (int)out_sz - 1;
    memcpy(out, close + 1, copy); out[copy] = '\0'; return true;
}
/* ---- end verbatim ---- */

static const char *BANDS[] = { "20M", "40M", "15M", "17M", "30M", "10M" };

/* One record in the firmware's own format, ~188 B like N4OPI's. */
static void make_line(char *dst, size_t n, int rec, int uniq_calls)
{
    unsigned c = (unsigned)(((unsigned)rec * 2654435761u) % (unsigned)uniq_calls);
    snprintf(dst, n,
        "<CALL:6>%c%c%u%c%c%c<FREQ:6>14.074<BAND:3>%s<MODE:3>FT8<RST_SENT:3>-09"
        "<RST_RCVD:3>-10<QSO_DATE:8>20261113<TIME_ON:6>%06u<STATION_CALLSIGN:5>N4OPI"
        "<MY_GRIDSQUARE:6>EM65NW<GRIDSQUARE:4>CN84<EOR>\n",
        'A' + (int)(c % 26), 'A' + (int)((c / 26) % 26), c % 10,
        'A' + (int)((c / 260) % 26), 'A' + (int)((c / 6760) % 26),
        'A' + (int)((c / 175760) % 26),
        BANDS[rec % 6], (unsigned)(rec % 1000000));
}

static double load(int records, int uniq_calls, int *out_pairs)
{
    free(s_worked); s_worked = NULL; s_worked_count = s_worked_cap = 0;
    char **lines = malloc(sizeof(char *) * records);
    for (int i = 0; i < records; i++) { lines[i] = malloc(256); make_line(lines[i], 256, i, uniq_calls); }
    clock_t t0 = clock();
    for (int i = 0; i < records; i++) {
        const char *line = lines[i];
        if (!strstr(line, "<EOR>")) continue;
        char call[ADIF_CALL_MAX], band[ADIF_BAND_MAX];
        if (extract_field(line, "CALL", call, sizeof(call))) {
            if (!extract_field(line, "BAND", band, sizeof(band))) band[0] = '\0';
            cache_add(call, band);
        }
    }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    for (int i = 0; i < records; i++) free(lines[i]);
    free(lines);
    *out_pairs = s_worked_count;
    return ms;
}

int main(void)
{
    /* N4OPI's boot log: `I (3957) adif: SPIFFS: qso.adi 511846 B` ->
     * `I (4985) adif: Loaded 2715 QSOs`.
     *
     * NOT A CPU SCALING FACTOR, and it must never be used as one. The same
     * work below costs well under a millisecond here. A gap of three orders of
     * magnitude is not a CPU difference - the device figure is dominated by
     * SPIFFS file I/O. That is itself the finding: at boot this is an I/O
     * problem first and an algorithm problem second, and both point the same
     * way. */
    const double DEV_MS_AT_2715 = 1028.0;
    int pairs;
    double href = 0;
    for (int r = 0; r < 5; r++) href += load(2715, 2000, &pairs);
    href /= 5;
    printf("device: 2,715 records in %.0f ms (measured, N4OPI)\n", DEV_MS_AT_2715);
    printf("host:   same work in %.2f ms  -> the device figure is file I/O, not this loop\n\n", href);

    printf("-- option 2: pre-deduplicated index, append without scanning --\n");
    printf("%10s %12s\n", "pairs", "host ms");
    g_nodedup = 1;
    for (int n = 5000; n <= 40000; n *= 2) {
        double h = 0;
        for (int r = 0; r < 3; r++) h += load(n, n, &pairs);
        printf("%10d %12.2f\n", n, h / 3);
    }
    g_nodedup = 0;

    printf("\n-- option 1: today's scan on insert --\n");
    printf("%10s %10s %12s\n", "records", "pairs", "host ms");
    int ns[] = { 700, 2715, 5000, 10000, 20000, 40000 };
    for (unsigned i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        int n = ns[i];
        int reps = n > 10000 ? 1 : 3;
        double h = 0;
        for (int r = 0; r < reps; r++) h += load(n, n, &pairs);
        printf("%10d %10d %12.2f\n", n, pairs, h / reps);
    }

    printf("\nBytes to read at boot, ~188 B per ADIF record vs 26 B per index entry:\n");
    for (int n = 5000; n <= 40000; n *= 2)
        printf("%10d: archive %7.2f MB   index %6.0f KB\n",
               n, n * 188.0 / 1048576.0, n * 26.0 / 1024.0);
    return 0;
}
