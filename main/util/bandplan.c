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
/* ⛔ THE TOP 500 Hz WAS MISSING, AND IT IS WHERE WSPR LIVES.
 *
 * The WRC-15 secondary allocation is 5351.5-5366.5 kHz. This table stopped at
 * 5366.0, which is where the IARU R1 BAND PLAN's all-modes segment ends - a
 * usage convention, not the licence edge. 5366.0-5366.5 is the narrowband /
 * weak-signal part, and WSPR at the conventional 5364.7 dial emits at
 * 5366.1-5366.3 kHz, i.e. inside the allocation but outside this table.
 *
 * Found 2026-10-21 checking Wolfgang DF1KZ's report: his suggested frequency
 * read as out of band against our own plan, which would have made the correct
 * number look illegal. Two defects, and fixing only the WSPR dial would have
 * left this one to contradict it. */
static const bp_seg_t R1_60[]  = {{5351500,5354000,BP_CW},{5354000,5366000,BP_PHONE},{5366000,5366500,BP_DIGI}};
static const bp_seg_t R1_40[]  = {{7000000,7040000,BP_CW},{7040000,7050000,BP_DIGI},{7050000,7200000,BP_PHONE}};
static const bp_seg_t R1_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R1_20[]  = {{14000000,14070000,BP_CW},{14070000,14100000,BP_DIGI},{14100000,14350000,BP_PHONE}};
static const bp_seg_t R1_17[]  = {{18068000,18095000,BP_CW},{18095000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R1_15[]  = {{21000000,21070000,BP_CW},{21070000,21150000,BP_DIGI},{21150000,21450000,BP_PHONE}};
static const bp_seg_t R1_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R1_11[]  = {{26900000,27500000,BP_PHONE}};  // 11m/CB: voice only, no CW/digi sub-bands
static const bp_seg_t R1_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};
static const bp_seg_t R1_6[]   = {{50000000,50100000,BP_CW},{50100000,50300000,BP_PHONE},{50300000,50400000,BP_DIGI},{50400000,52000000,BP_PHONE}};

// ---- Region 2 (Americas, US-centric coarse) ------------------------------
static const bp_seg_t R2_160[] = {{1800000,1843000,BP_CW},{1843000,2000000,BP_PHONE}};
static const bp_seg_t R2_80[]  = {{3500000,3570000,BP_CW},{3570000,3600000,BP_DIGI},{3600000,4000000,BP_PHONE}};
/* ⛔ REWRITTEN 2026-10-21 FROM THE FCC ORDER, NOT FROM A USER'S VIEW.
 *
 * I had this as one continuous 5330.5-5366.5 block and was going to ASK a US
 * operator what the right WSPR frequency was. Wrong on both counts - his
 * correction: "WE need to be the ones that know the rules - its not up for
 * viewpoints." A regulation is a fact to look up.
 *
 * ⭐ AND THE RULES HAD CHANGED EIGHT MONTHS AGO. FCC 25-60 (adopted
 * 2025-09-23, released 2025-12-09) took effect 13 FEBRUARY 2026: the US now
 * has the WRC-15 contiguous band 5351.5-5366.5 kHz at 9.15 W ERP, max 2.8 kHz
 * bandwidth, General and above - PLUS four of the five legacy channels, kept
 * at 100 W ERP. The old 5358.5 kHz channel is gone because it falls inside the
 * new segment. So "the US has five discrete channels" was stale, and the
 * channel list I was about to defend no longer existed.
 *
 * Legacy channel CENTRES 5332 / 5348 / 5373 / 5405 kHz. In USB the suppressed
 * carrier sits 1.5 kHz below the centre and the emission may be 2.8 kHz wide,
 * so each channel occupies centre-1500 .. centre+1300.
 *
 * ⚠ REGION 2 IS NOT THE USA. Canada, Mexico and South America have the
 * WRC-15 segment and NOT the four US channels, so this table is permissive for
 * them by four narrow slices. Deliberate while the band plan is keyed by
 * REGION and 60 m is granted by COUNTRY: with the TX guard refusing what falls
 * outside, over-permission on four channels is the safer error than refusing a
 * US operator on a channel they are licensed for.
 *
 * ⚠ The 9.15 W ERP limit in the new segment is NOT enforced here - this
 * table is frequencies. A QMX at 5 W is inside it anyway. */
static const bp_seg_t R2_60_CH1[] = {{5330500,5333300,BP_PHONE}};   /* ch 5332 */
static const bp_seg_t R2_60_CH2[] = {{5346500,5349300,BP_PHONE}};   /* ch 5348 */
static const bp_seg_t R2_60[]  = {{5351500,5354000,BP_CW},{5354000,5366000,BP_PHONE},{5366000,5366500,BP_DIGI}};
static const bp_seg_t R2_60_CH4[] = {{5371500,5374300,BP_PHONE}};   /* ch 5373 */
static const bp_seg_t R2_60_CH5[] = {{5403500,5406300,BP_PHONE}};   /* ch 5405 */
static const bp_seg_t R2_40[]  = {{7000000,7070000,BP_CW},{7070000,7125000,BP_DIGI},{7125000,7300000,BP_PHONE}};
static const bp_seg_t R2_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R2_20[]  = {{14000000,14070000,BP_CW},{14070000,14150000,BP_DIGI},{14150000,14350000,BP_PHONE}};
static const bp_seg_t R2_17[]  = {{18068000,18100000,BP_CW},{18100000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R2_15[]  = {{21000000,21070000,BP_CW},{21070000,21200000,BP_DIGI},{21200000,21450000,BP_PHONE}};
static const bp_seg_t R2_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R2_11[]  = {{26900000,27500000,BP_PHONE}};  // 11m/CB: voice only, no CW/digi sub-bands
static const bp_seg_t R2_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};
static const bp_seg_t R2_6[]   = {{50000000,50100000,BP_CW},{50100000,50300000,BP_PHONE},{50300000,50400000,BP_DIGI},{50400000,54000000,BP_PHONE}};

// ---- Region 3 (Asia-Pacific) — close to R1 with minor edge differences ---
static const bp_seg_t R3_160[] = {{1800000,1838000,BP_CW},{1838000,1843000,BP_DIGI},{1843000,2000000,BP_PHONE}};
static const bp_seg_t R3_80[]  = {{3500000,3535000,BP_CW},{3535000,3580000,BP_DIGI},{3580000,3900000,BP_PHONE}};
static const bp_seg_t R3_60[]  = {{5351500,5354000,BP_CW},{5354000,5366000,BP_PHONE},{5366000,5366500,BP_DIGI}};  /* see R1_60 */
static const bp_seg_t R3_40[]  = {{7000000,7040000,BP_CW},{7040000,7050000,BP_DIGI},{7050000,7300000,BP_PHONE}};
static const bp_seg_t R3_30[]  = {{10100000,10130000,BP_CW},{10130000,10150000,BP_DIGI}};
static const bp_seg_t R3_20[]  = {{14000000,14070000,BP_CW},{14070000,14112000,BP_DIGI},{14112000,14350000,BP_PHONE}};
static const bp_seg_t R3_17[]  = {{18068000,18095000,BP_CW},{18095000,18110000,BP_DIGI},{18110000,18168000,BP_PHONE}};
static const bp_seg_t R3_15[]  = {{21000000,21070000,BP_CW},{21070000,21150000,BP_DIGI},{21150000,21450000,BP_PHONE}};
static const bp_seg_t R3_12[]  = {{24890000,24915000,BP_CW},{24915000,24930000,BP_DIGI},{24930000,24990000,BP_PHONE}};
static const bp_seg_t R3_11[]  = {{26900000,27500000,BP_PHONE}};  // 11m/CB: voice only, no CW/digi sub-bands
static const bp_seg_t R3_10[]  = {{28000000,28070000,BP_CW},{28070000,28300000,BP_DIGI},{28300000,29700000,BP_PHONE}};
static const bp_seg_t R3_6[]   = {{50000000,50100000,BP_CW},{50100000,50300000,BP_PHONE},{50300000,50400000,BP_DIGI},{50400000,54000000,BP_PHONE}};

#define BAND(arr) (arr), (int)(sizeof(arr)/sizeof((arr)[0]))

static const bp_band_t R1_BANDS[] = {
    {1810000,2000000, BAND(R1_160)}, {3500000,3800000, BAND(R1_80)},
    {5351500,5366500, BAND(R1_60)},  {7000000,7200000, BAND(R1_40)},
    {10100000,10150000,BAND(R1_30)}, {14000000,14350000,BAND(R1_20)},
    {18068000,18168000,BAND(R1_17)}, {21000000,21450000,BAND(R1_15)},
    {24890000,24990000,BAND(R1_12)}, {26900000,27500000,BAND(R1_11)}, {28000000,29700000,BAND(R1_10)},
    {50000000,52000000,BAND(R1_6)},
};
static const bp_band_t R2_BANDS[] = {
    {1800000,2000000, BAND(R2_160)}, {3500000,4000000, BAND(R2_80)},
    /* 60 m is FIVE disjoint entries in Region 2 - see R2_60 above. The gaps
     * between them (5333.3-5346.5, 5349.3-5351.5, 5366.5-5371.5,
     * 5374.3-5403.5) carry no amateur allocation, and the old single block
     * wrongly allowed two of them while omitting the 5373 and 5405 channels
     * altogether. */
    {5330500,5333300, BAND(R2_60_CH1)},
    {5346500,5349300, BAND(R2_60_CH2)},
    {5351500,5366500, BAND(R2_60)},
    {5371500,5374300, BAND(R2_60_CH4)},
    {5403500,5406300, BAND(R2_60_CH5)},
    {7000000,7300000, BAND(R2_40)},
    {10100000,10150000,BAND(R2_30)}, {14000000,14350000,BAND(R2_20)},
    {18068000,18168000,BAND(R2_17)}, {21000000,21450000,BAND(R2_15)},
    {24890000,24990000,BAND(R2_12)}, {26900000,27500000,BAND(R2_11)}, {28000000,29700000,BAND(R2_10)},
    {50000000,54000000,BAND(R2_6)},
};
static const bp_band_t R3_BANDS[] = {
    {1800000,2000000, BAND(R3_160)}, {3500000,3900000, BAND(R3_80)},
    {5351500,5366500, BAND(R3_60)},  {7000000,7300000, BAND(R3_40)},
    {10100000,10150000,BAND(R3_30)}, {14000000,14350000,BAND(R3_20)},
    {18068000,18168000,BAND(R3_17)}, {21000000,21450000,BAND(R3_15)},
    {24890000,24990000,BAND(R3_12)}, {26900000,27500000,BAND(R3_11)}, {28000000,29700000,BAND(R3_10)},
    {50000000,54000000,BAND(R3_6)},
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

bool bandplan_region_is_known(bandplan_region_t setting, const char *grid)
{
    if (setting == BP_REGION_1 || setting == BP_REGION_2 || setting == BP_REGION_3)
        return true;
    double lat, lon;
    return grid && maidenhead_to_latlon(grid, &lat, &lon);
}

bool bandplan_tx_in_band(uint32_t rf_lo_hz, uint32_t rf_hi_hz,
                         bandplan_region_t region)
{
    if (rf_hi_hz < rf_lo_hz) { uint32_t t = rf_lo_hz; rf_lo_hz = rf_hi_hz; rf_hi_hz = t; }
    const bp_band_t *tbl;
    int n;
    region_table(region, &tbl, &n);
    for (int i = 0; i < n; i++) {
        /* BOTH edges in the SAME band. Testing them separately would pass a
         * burst that straddles two bands, and testing only the centre would
         * pass one whose top tone is outside. */
        if (rf_lo_hz >= tbl[i].lo_hz && rf_hi_hz <= tbl[i].hi_hz) return true;
    }
    return false;
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
