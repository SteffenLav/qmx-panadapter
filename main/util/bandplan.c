// Coarse amateur band-plan data. See bandplan.h for the contract and the
// "this is a hint, not a legal authority" caveat. Numbers are approximate
// IARU/region conventions, intentionally coarse (CW / DIGI / PHONE blocks).

#include "bandplan.h"
#include "maidenhead.h"

#include <stddef.h>

// One band's coarse plan for one region: the band edges plus its segment list.
typedef struct {
    uint32_t        lo_hz;
    uint32_t        hi_hz;
    const bp_seg_t *segs;
    int             n_segs;
} bp_band_t;

// ---- Region 1 (Europe / Africa / Middle East) ----------------------------
static const bp_seg_t R1_160[] = {{1810000,1838000,BP_CW},{1838000,1843000,BP_DIGI},{1843000,2000000,BP_PHONE}};
static const bp_seg_t R1_80[]  = {{3500000,3580000,BP_CW},{3580000,3600000,BP_DIGI},{3600000,3800000,BP_PHONE}};
static const bp_seg_t R1_60[]  = {{5351500,5354000,BP_CW},{5354000,5366000,BP_PHONE}};
static const bp_seg_t R1_40[]  = {{7000000,7040000,BP_CW},{7040000,7050000,BP_DIGI},{7050000,7200000,BP_PHONE}};
static const bp_seg_t R1_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R1_20[]  = {{14000000,14070000,BP_CW},{14070000,14100000,BP_DIGI},{14100000,14350000,BP_PHONE}};
static const bp_seg_t R1_17[]  = {{18068000,18095000,BP_CW},{18095000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R1_15[]  = {{21000000,21070000,BP_CW},{21070000,21150000,BP_DIGI},{21150000,21450000,BP_PHONE}};
static const bp_seg_t R1_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R1_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};

// ---- Region 2 (Americas, US-centric coarse) ------------------------------
static const bp_seg_t R2_160[] = {{1800000,1843000,BP_CW},{1843000,2000000,BP_PHONE}};
static const bp_seg_t R2_80[]  = {{3500000,3570000,BP_CW},{3570000,3600000,BP_DIGI},{3600000,4000000,BP_PHONE}};
static const bp_seg_t R2_60[]  = {{5330500,5366500,BP_PHONE}};
static const bp_seg_t R2_40[]  = {{7000000,7070000,BP_CW},{7070000,7125000,BP_DIGI},{7125000,7300000,BP_PHONE}};
static const bp_seg_t R2_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R2_20[]  = {{14000000,14070000,BP_CW},{14070000,14150000,BP_DIGI},{14150000,14350000,BP_PHONE}};
static const bp_seg_t R2_17[]  = {{18068000,18100000,BP_CW},{18100000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R2_15[]  = {{21000000,21070000,BP_CW},{21070000,21200000,BP_DIGI},{21200000,21450000,BP_PHONE}};
static const bp_seg_t R2_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R2_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};

// ---- Region 3 (Asia-Pacific) — close to R1 with minor edge differences ---
static const bp_seg_t R3_160[] = {{1800000,1838000,BP_CW},{1838000,1843000,BP_DIGI},{1843000,2000000,BP_PHONE}};
static const bp_seg_t R3_80[]  = {{3500000,3535000,BP_CW},{3535000,3580000,BP_DIGI},{3580000,3900000,BP_PHONE}};
static const bp_seg_t R3_60[]  = {{5351500,5354000,BP_CW},{5354000,5366000,BP_PHONE}};
static const bp_seg_t R3_40[]  = {{7000000,7040000,BP_CW},{7040000,7050000,BP_DIGI},{7050000,7300000,BP_PHONE}};
static const bp_seg_t R3_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R3_20[]  = {{14000000,14070000,BP_CW},{14070000,14112000,BP_DIGI},{14112000,14350000,BP_PHONE}};
static const bp_seg_t R3_17[]  = {{18068000,18095000,BP_CW},{18095000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R3_15[]  = {{21000000,21070000,BP_CW},{21070000,21150000,BP_DIGI},{21150000,21450000,BP_PHONE}};
static const bp_seg_t R3_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R3_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};

#define BAND(arr) (arr), (int)(sizeof(arr)/sizeof((arr)[0]))

static const bp_band_t R1_BANDS[] = {
    {1810000,2000000, BAND(R1_160)}, {3500000,3800000, BAND(R1_80)},
    {5351500,5366000, BAND(R1_60)},  {7000000,7200000, BAND(R1_40)},
    {10100000,10150000,BAND(R1_30)}, {14000000,14350000,BAND(R1_20)},
    {18068000,18168000,BAND(R1_17)}, {21000000,21450000,BAND(R1_15)},
    {24890000,24990000,BAND(R1_12)}, {28000000,29700000,BAND(R1_10)},
};
static const bp_band_t R2_BANDS[] = {
    {1800000,2000000, BAND(R2_160)}, {3500000,4000000, BAND(R2_80)},
    {5330500,5366500, BAND(R2_60)},  {7000000,7300000, BAND(R2_40)},
    {10100000,10150000,BAND(R2_30)}, {14000000,14350000,BAND(R2_20)},
    {18068000,18168000,BAND(R2_17)}, {21000000,21450000,BAND(R2_15)},
    {24890000,24990000,BAND(R2_12)}, {28000000,29700000,BAND(R2_10)},
};
static const bp_band_t R3_BANDS[] = {
    {1800000,2000000, BAND(R3_160)}, {3500000,3900000, BAND(R3_80)},
    {5351500,5366000, BAND(R3_60)},  {7000000,7300000, BAND(R3_40)},
    {10100000,10150000,BAND(R3_30)}, {14000000,14350000,BAND(R3_20)},
    {18068000,18168000,BAND(R3_17)}, {21000000,21450000,BAND(R3_15)},
    {24890000,24990000,BAND(R3_12)}, {28000000,29700000,BAND(R3_10)},
};

static void region_table(bandplan_region_t r, const bp_band_t **tbl, int *n)
{
    switch (r) {
        case BP_REGION_2: *tbl = R2_BANDS; *n = (int)(sizeof(R2_BANDS)/sizeof(R2_BANDS[0])); break;
        case BP_REGION_3: *tbl = R3_BANDS; *n = (int)(sizeof(R3_BANDS)/sizeof(R3_BANDS[0])); break;
        case BP_REGION_1:
        default:          *tbl = R1_BANDS; *n = (int)(sizeof(R1_BANDS)/sizeof(R1_BANDS[0])); break;
    }
}

bandplan_region_t bandplan_region_from_grid(const char *grid)
{
    double lat, lon;
    if (!grid || !maidenhead_to_latlon(grid, &lat, &lon)) return BP_REGION_1;
    // Coarse longitude split. The Americas (R2) span roughly -170..-34; Europe/
    // Africa/Middle East (R1) roughly -34..+60; the rest is Asia-Pacific (R3).
    if (lon >= -170.0 && lon < -34.0) return BP_REGION_2;
    if (lon >= -34.0  && lon <  60.0) return BP_REGION_1;
    return BP_REGION_3;
}

bandplan_region_t bandplan_effective_region(bandplan_region_t setting, const char *grid)
{
    if (setting == BP_REGION_1 || setting == BP_REGION_2 || setting == BP_REGION_3)
        return setting;
    return bandplan_region_from_grid(grid);
}

int bandplan_get_segments(uint32_t freq_hz, bandplan_region_t region, const bp_seg_t **out)
{
    if (out) *out = NULL;
    const bp_band_t *tbl;
    int n;
    region_table(region, &tbl, &n);
    for (int i = 0; i < n; i++) {
        if (freq_hz >= tbl[i].lo_hz && freq_hz <= tbl[i].hi_hz) {
            if (out) *out = tbl[i].segs;
            return tbl[i].n_segs;
        }
    }
    return 0;
}

// Dimmed ~30% from the original bright 0x33AAFF/0xFFAA33/0x33CC55 (too
// bright against the waterfall, 2026-06-30 feedback). Kept in sync with
// ui_theme.h's UI_COLOR_MODE_CW/UI_COLOR_MODE_DIGI - the memory-channel grid
// and the freq pad's mode row use the same CW/DiGi colours as this strip.
uint32_t bandplan_seg_color(bp_seg_type_t type)
{
    switch (type) {
        case BP_CW:    return 0x2477B3;  // blue  — CW
        case BP_DIGI:  return 0xB37724;  // amber — digital
        case BP_PHONE: return 0x248F3C;  // green — phone/SSB
        default:       return 0x808080;
    }
}

const char *bandplan_seg_label(bp_seg_type_t type)
{
    switch (type) {
        case BP_CW:    return "CW";
        case BP_DIGI:  return "Digi";
        case BP_PHONE: return "Phone";
        default:       return "";
    }
}
