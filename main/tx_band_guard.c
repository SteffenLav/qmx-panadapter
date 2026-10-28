// See tx_band_guard.h for what this refuses and, more importantly, for the
// three cases it deliberately does not.

#include "tx_band_guard.h"

#include <stdio.h>

#include "esp_log.h"

#include "cat/cat.h"
#include "storage/settings.h"
#include "util/bandplan.h"

static const char *TAG = "txband";

bool tx_band_guard_allows(uint32_t rf_lo_hz, uint32_t rf_hi_hz,
                          const char *what, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!what) what = "TX";

    if (settings_get_tx_band_guard_off()) return true;

    /* ⚠ A dial of 0 means CAT has not told us where the radio is, not that the
     * radio is at 0 Hz. Refusing here would block every transmission whenever
     * the link is flaky - and a flaky link is already its own failure, with its
     * own reporting (see wspr_tx.c's key-down verification). */
    if (rf_lo_hz == 0 || rf_hi_hz == 0) return true;

    qmx_settings_t c;
    settings_load_all(&c);

    /* ⛔ AN UNKNOWN REGION REFUSES. Operator's call, 2026-10-21: "I would
     * argue that the region is just as important as the call sign and grid to
     * do any tx'ing" - and both paths already refuse without those two
     * ("Set your callsign and grid first"), so this joins that list rather
     * than inventing a rule.
     *
     * ⭐ IT IS ALSO THE SAFER OF THE TWO WRONG ANSWERS. My first version let
     * an unknown region through unchecked, to avoid judging a US operator
     * against Region 1 - because bandplan_region_from_grid() returns R1 for a
     * missing grid and would refuse them on 3.9 MHz, which is legal there.
     * But passing it through means transmitting with no check at all, which is
     * the very hole this guard exists to close. Refusing makes the gap VISIBLE
     * and fixable in one setting, where the other choice is silently wrong in
     * one direction or the other.
     *
     * NOT bandplan_effective_region() on its own: that cannot tell a region
     * the operator chose from one it fell back to. */
    if (!bandplan_region_is_known((bandplan_region_t)c.bandplan_region, c.my_grid)) {
        ESP_LOGE(TAG, "%s REFUSED: no IARU region - it is on Auto and no usable grid "
                      "is set, so the band cannot be checked. Nothing was transmitted.",
                 what);
        if (err && err_len)
            snprintf(err, err_len,
                     "%s not sent: set the IARU region first (Settings), or let the "
                     "GPS set your grid.", what);
        return false;
    }

    bandplan_region_t reg =
        bandplan_effective_region((bandplan_region_t)c.bandplan_region, c.my_grid);
    if (bandplan_tx_in_band(rf_lo_hz, rf_hi_hz, reg)) return true;

    const int rn = (reg == BP_REGION_2) ? 2 : (reg == BP_REGION_3) ? 3 : 1;
    ESP_LOGE(TAG, "%s REFUSED: %lu.%06lu MHz is outside the IARU Region %d "
                  "allocation - nothing was transmitted",
             what, (unsigned long)(rf_lo_hz / 1000000u),
             (unsigned long)(rf_lo_hz % 1000000u), rn);
    if (err && err_len)
        /* The frequency and the region both, because "out of band" without the
         * region is unactionable - the commonest cause is the wrong region set,
         * not the wrong frequency. */
        snprintf(err, err_len,
                 "%s not sent: %lu.%06lu MHz is outside the Region %d band plan. "
                 "Check the frequency, or the IARU region in Settings.",
                 what, (unsigned long)(rf_lo_hz / 1000000u),
                 (unsigned long)(rf_lo_hz % 1000000u), rn);
    return false;
}
