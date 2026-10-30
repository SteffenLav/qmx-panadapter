#include "js8_alt.h"

/* See js8_alt.h for why this file holds nothing but arithmetic. */

int js8_alt_slots_in_primary(int64_t slot_sec, int primary_s, int alt_s,
                             int *offs_samples, int max_offs)
{
    if (primary_s <= 0 || alt_s <= 0) return 0;

    /* The second within the minute. Every submode period divides 60 s and
     * every slot grid is anchored to the UTC epoch, so the phase of the
     * minute is the whole of what the alignment depends on - working modulo
     * 60 keeps the arithmetic in `int` and makes the harness able to ask
     * about any t0 directly. */
    int t0 = (int)(((slot_sec % 60) + 60) % 60);

    int n = 0;
    for (int t = ((t0 + alt_s - 1) / alt_s) * alt_s;
         t + alt_s <= t0 + primary_s; t += alt_s) {
        if (offs_samples && n < max_offs)
            offs_samples[n] = (t - t0) * JS8_ALT_SR_HZ;
        n++;
    }
    return n;
}

bool js8_alt_fits(ftx_protocol_t primary, ftx_protocol_t alt)
{
    if (!ftx_protocol_is_js8(primary) || !ftx_protocol_is_js8(alt)) return false;
    if (primary == alt) return false;
    /* A slot longer than the capture can never fit, whatever the phase - this
     * is what excludes Slow from a 15 s primary. The loop below would say so
     * too, but stating it here keeps the reason in one place. */
    return ftx_protocol_slot_ms(alt) <= ftx_protocol_slot_ms(primary);
}

bool js8_alt_slot_due(int64_t slot_sec, int primary_s)
{
    if (primary_s <= 0) return false;
    int64_t n = slot_sec / primary_s;
    return ((n % JS8_ALT_DUTY_EVERY) + JS8_ALT_DUTY_EVERY) % JS8_ALT_DUTY_EVERY == 0;
}

ftx_protocol_t js8_alt_proto_for_submode(uint8_t nsubmode)
{
    switch (nsubmode) {
    case 1:  return FTX_PROTOCOL_JS8_FAST;
    case 2:  return FTX_PROTOCOL_JS8_TURBO;
    case 4:  return FTX_PROTOCOL_JS8_SLOW;
    default: return FTX_PROTOCOL_JS8;
    }
}

bool js8_alt_select(ftx_protocol_t primary, uint8_t alt_submode,
                    int64_t slot_sec,
                    ftx_protocol_t *alt, int *offs_samples, int max_offs,
                    int *n_windows)
{
    if (alt_submode == JS8_ALT_OFF) return false;
    if (!ftx_protocol_is_js8(primary)) return false;

    ftx_protocol_t a = js8_alt_proto_for_submode(alt_submode);
    if (!js8_alt_fits(primary, a)) return false;

    int primary_s = ftx_protocol_slot_ms(primary) / 1000;
    int alt_s     = ftx_protocol_slot_ms(a) / 1000;
    if (!js8_alt_slot_due(slot_sec, primary_s)) return false;

    int n = js8_alt_slots_in_primary(slot_sec, primary_s, alt_s,
                                     offs_samples, max_offs);
    if (n <= 0) return false;
    if (n > max_offs) n = max_offs;

    if (alt)       *alt = a;
    if (n_windows) *n_windows = n;
    return true;
}

char js8_alt_mark(ftx_protocol_t proto, ftx_protocol_t primary)
{
    if (proto == primary) return ' ';
    switch (proto) {
    case FTX_PROTOCOL_JS8:       return 'N';
    case FTX_PROTOCOL_JS8_FAST:  return 'F';
    case FTX_PROTOCOL_JS8_TURBO: return 'T';
    case FTX_PROTOCOL_JS8_SLOW:  return 'S';
    default:                     return ' ';
    }
}
