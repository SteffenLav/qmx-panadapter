#include "cat_restore.h"

static bool mode_known(char d) { return d >= '1' && d <= '9'; }

cat_restore_plan_t cat_restore_plan(uint32_t pre_freq, char pre_mode,
                                    uint32_t now_freq, char now_mode)
{
    cat_restore_plan_t p = {0};

    // No answer to the pre-scan query means we do not know where the radio was.
    // Writing anything here would be inventing a dial position.
    if (pre_freq && now_freq != pre_freq) {
        p.send_freq = true;
        p.freq_hz   = pre_freq;
    }

    if (mode_known(pre_mode) && now_mode != pre_mode) {
        p.send_mode  = true;
        p.mode_digit = pre_mode;
    }

    return p;
}
