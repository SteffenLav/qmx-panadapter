// sb_tune.h - estimate where a single-sideband signal's suppressed carrier is,
// from one window of the spectrum. T90, the SSB sibling of the CW zero-beat.
//
// ⛔ NOT YET IN THE FIRMWARE BUILD. main/CMakeLists.txt lists its sources
// explicitly, and this file is deliberately not in that list: the design is
// being measured on the host first (test/sb_tune_harness.c) because the thing
// that decides whether it works at all is a BIAS, not a bug, and a bias is
// invisible on the glass. Add it to SRCS only once the harness says the total
// error fits inside half a tune-snap step.
//
// WHY THIS IS NOT ui.c's cw_peak_in()
//   CW has a carrier, so "tune to the peak" is the whole job and ui.c does it
//   well (median noise reference, 3 dB gate, parabolic interpolation). SSB has
//   no carrier: the peak is wherever the speaker's energy happens to sit this
//   instant, and it moves with every syllable. What stays put is the EDGE of
//   the occupied band at the suppressed-carrier end - the low edge for USB,
//   the high edge for LSB.
//
// ⚠ THE EDGE IS NOT THE CARRIER, and that is the central problem.
//   Voice has no energy below roughly 300 Hz, and the transmitting station's
//   own filter cuts there too, so the edge sits a few hundred Hz inside the
//   carrier. That offset belongs to THEIR rig (300 Hz is common, some run 200,
//   some 100) and we cannot know it. On top of it the analysis window smears
//   the edge: at DSP_FFT_SIZE 1024 and 48 kHz a bin is 46.875 Hz, and the
//   default Blackman-Harris main lobe is about four bins, so ~190 Hz of the
//   apparent slope is the window rather than the signal.
//
//   This estimator therefore does NOT claim to find the carrier. It finds the
//   edge, reports it, and lets the caller add a calibration offset and snap to
//   the operator's tune grid - which is what makes the whole idea practical:
//   there is no absolute correct SSB dial, only the frequency the other
//   operator chose, and they are almost certainly on a round number.
//
// Portable on purpose: <math.h> and nothing else. No LVGL, no ESP-IDF, no
// esp-dsp - so test/sb_tune_harness.c links THIS code rather than a copy of
// it, the same portable-file split util/format_freq.c and util/db_gridlines.c
// already use.

#ifndef SB_TUNE_H
#define SB_TUNE_H

#include <stdbool.h>

typedef enum {
    SB_TUNE_OK = 0,
    SB_TUNE_NO_SIGNAL,      // nothing in the window clears the SNR gate
    SB_TUNE_EDGE_NOT_FOUND, // signal present but no clean threshold crossing
} sb_tune_result_t;

typedef struct {
    // In-band level must beat the noise reference by this much before the
    // window is considered to hold a signal at all. ui.c's CW gate is 3 dB;
    // speech is burstier than a carrier, so the default here is higher.
    float min_snr_db;

    // The edge is taken where the spectrum crosses this far below the in-band
    // reference level. Shallow values chase noise; deep values walk out into
    // the window's own skirt. THE HARNESS SWEEPS THIS - do not tune it by eye.
    float edge_drop_db;

    // How many of the strongest bins form the in-band reference. A single peak
    // bin is a syllable, not a level, so the reference is the median of the
    // strongest few.
    int inband_bins;

    /* ⛔ THE FIT'S OWN GATE, AND IT IS A SEPARATE FIELD ON PURPOSE.
     *
     * sb_tune_find_edge_fit() measures confidence as the height of the fitted
     * step - a difference of MEANS in dB - which is a fundamentally smaller
     * number than min_snr_db's peak-to-floor percentile gap, because between
     * voice harmonics the spectrum sits at the noise floor and drags the
     * signal-side mean down. Feeding the fit a min_snr_db of 25 refused every
     * case at every SNR. Two estimators, two scales, two fields; sharing one
     * number cost a cycle and would cost the next reader more.
     *
     * 11.5 dB is measured, not chosen. Across the synthetic sweep the step
     * runs 1.0 dB mean (p90 5.3) for signals at 10-20 dB SNR, 7.8 dB mean
     * (p90 11.5) at 20-30 dB, and 18.7 dB mean (p10 15.9) above 30 dB. It is
     * set to that 20-30 dB p90: a first try at 13 dB cleared only 91.2 % of
     * the 30-40 dB band against Gate 1's 95 % criterion, and the table says
     * where the room is. */
    float min_step_db;
} sb_tune_cfg_t;

// The edge threshold must clear the noise reference by this much or the window
// is refused outright. See the long note in sb_tune.c: the version that CLAMPED
// to the noise instead of refusing produced confident wrong answers at 10-20 dB
// SNR and never once said no.
#define SB_TUNE_FLOOR_MARGIN_DB  3.0f

// ⚠ min_snr_db is 25, not the 6 this started with, and it is a MEASURED value:
// at 10 and 20 dB the estimator stopped tracking the far station's filter
// altogether (edge near -50 Hz whatever the filter, sd > 200 Hz), while at
// 30 dB it tracked with sd under 55 Hz. A weak SSB signal is not tunable by
// this method and the gate is where that is admitted.
#define SB_TUNE_CFG_DEFAULT  ((sb_tune_cfg_t){ \
    .min_snr_db   = 25.0f,                     \
    .edge_drop_db = 6.0f,                       \
    .inband_bins  = 5,                         \
    .min_step_db  = 11.5f,                     \
})

// win_db      per-bin power in dB, low frequency first
// n           bins in the window
// centre_i    index of the dial's own frequency within the window
// bin_hz      Hz per bin
// upper       true for USB (carrier at the LOW edge), false for LSB
// edge_off_hz out: edge position relative to the dial, signed
// snr_db      out: in-band reference minus noise reference; may be NULL
//
// The return value is the only thing that says whether the outputs mean
// anything. On anything but SB_TUNE_OK they are left untouched.
sb_tune_result_t sb_tune_find_edge(const float *win_db, int n, int centre_i,
                                   float bin_hz, bool upper,
                                   const sb_tune_cfg_t *cfg,
                                   float *edge_off_hz, float *snr_db);

/* ⭐ THE SHAPE FIT - a changepoint, not a threshold crossing.
 *
 * WHY THE THRESHOLD VERSION ABOVE IS NOT ENOUGH, measured on the bench:
 * real 40 m SSB presents about 19 dB median and 26 dB p90 per-bin SNR against
 * a genuine noise floor, while sb_tune_find_edge() needs about 30 dB to track
 * the far station's filter. Three ways of closing that gap were measured and
 * all three failed - a stronger station (+4.3 dB), a 4 s per-bin max-hold
 * (p90 moved 20 -> 23 dB), and a full-span noise reference (+1.5 dB).
 *
 * The reason is that a threshold crossing decides the answer from ONE bin's
 * worth of evidence. This fits the carrier-side profile as two levels - noise,
 * then signal - and picks the transition that minimises the total squared
 * residual, so every bin in the window votes. Noise averages down instead of
 * moving the answer.
 *
 * It needs no quiet reference and no threshold: the fitted STEP HEIGHT is the
 * confidence, and cfg->min_snr_db is read as the smallest step worth
 * believing. cfg->edge_drop_db and cfg->inband_bins are unused here.
 *
 * Same arguments and same contract as sb_tune_find_edge(): on anything but
 * SB_TUNE_OK the outputs are untouched, and *snr_db (when asked for) is the
 * fitted step height rather than a percentile difference.
 */
sb_tune_result_t sb_tune_find_edge_fit(const float *win_db, int n, int centre_i,
                                       float bin_hz, bool upper,
                                       const sb_tune_cfg_t *cfg,
                                       float *edge_off_hz, float *snr_db);

// Round a dial to the operator's tune grid. grid_hz of 0 means "no snapping"
// and returns freq_hz unchanged, matching settings' tune_snap_hz where 0 is
// off and the offered values are 250 / 500 / 1000.
unsigned sb_tune_snap(unsigned freq_hz, unsigned grid_hz);

#endif // SB_TUNE_H
