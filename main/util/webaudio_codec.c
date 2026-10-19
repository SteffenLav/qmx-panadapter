#include "webaudio_codec.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void webaud_decim_init(webaud_decim_t *st, int fs_in_hz)
{
    memset(st, 0, sizeof(*st));
    const float fc = (float)WEBAUD_FC_HZ / (float)fs_in_hz;   /* cycles per sample */
    const int   m  = WEBAUD_FIR_LEN / 2;
    float sum = 0.0f;
    for (int i = 0; i < WEBAUD_FIR_LEN; i++) {
        int   k = i - m;
        float s = (k == 0) ? (2.0f * fc)
                           : sinf(2.0f * (float)M_PI * fc * (float)k) /
                             ((float)M_PI * (float)k);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i /
                                       (float)(WEBAUD_FIR_LEN - 1));
        st->fir[i] = s * w;
        sum += st->fir[i];
    }
    /* Unity at DC, not unity energy: the input is already at the level the
       operator set with the AGC and the volume, and a gain change here would
       make the browser and the speaker disagree about how loud the band is. */
    for (int i = 0; i < WEBAUD_FIR_LEN; i++) st->fir[i] /= sum;
}

void webaud_decim_reset(webaud_decim_t *st)
{
    memset(st->hist, 0, sizeof(st->hist));
    st->hi = 0;
    st->phase = 0;
}

bool webaud_decim_push(webaud_decim_t *st, float mono, int16_t *out)
{
    st->hist[st->hi] = mono;
    st->hi = (st->hi + 1) % WEBAUD_FIR_LEN;
    if (++st->phase < WEBAUD_DECIM) return false;
    st->phase = 0;

    float acc = 0.0f;
    int   h   = st->hi;                     /* oldest sample */
    for (int t = 0; t < WEBAUD_FIR_LEN; t++) {
        acc += st->fir[WEBAUD_FIR_LEN - 1 - t] * st->hist[h];
        h = (h + 1) % WEBAUD_FIR_LEN;
    }
    if (acc >  32767.0f) acc =  32767.0f;
    if (acc < -32768.0f) acc = -32768.0f;
    *out = (int16_t)acc;
    return true;
}

/* ⛔ THE SUN REFERENCE ALGORITHM, NOT A 16-BIT REWORKING OF IT.
 *
 * The first version here worked on the 16-bit value directly with the
 * constants scaled up (CLIP 32635, BIAS 0x84) - a form that appears in plenty
 * of codebases and looks equivalent. The harness compared it against Sun's
 * g711.c for all 65,536 inputs: 381 DISAGREEMENTS, the first at pcm=-31611.
 * They are all in the top segment and all off by one code, so it would have
 * decoded as audio and sounded fine; it simply would not have been mu-law, and
 * nothing downstream could ever have told anyone.
 *
 * The difference is the >> 2. The reference quantises to 14 bits BEFORE the
 * segment search, so the rounding at a segment boundary goes the other way.
 * Follow the reference. */
uint8_t webaud_mulaw_encode(int16_t pcm)
{
    static const int32_t seg_uend[8] = {
        0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF
    };
    int32_t v = pcm >> 2;              /* 14-bit, as the reference does */
    int32_t mask;
    if (v < 0) { v = -v; mask = 0x7F; } else { mask = 0xFF; }
    if (v > 8159) v = 8159;            /* CLIP */
    v += (0x84 >> 2);                  /* BIAS */

    int seg = 0;
    while (seg < 8 && v > seg_uend[seg]) seg++;
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    return (uint8_t)((((seg << 4) | ((v >> (seg + 1)) & 0x0F))) ^ mask);
}

int16_t webaud_mulaw_decode(uint8_t u)
{
    const int32_t BIAS = 0x84;
    u = (uint8_t)~u;
    int32_t sign     = u & 0x80;
    int32_t exponent = (u >> 4) & 0x07;
    int32_t mantissa = u & 0x0F;
    int32_t v = ((mantissa << 3) + BIAS) << exponent;
    v -= BIAS;
    return (int16_t)(sign ? -v : v);
}
