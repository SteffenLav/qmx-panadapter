#include "wspr_wav.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* Little-endian stores, written out rather than memcpy'd from host integers,
 * so the layout does not silently depend on the build machine's endianness.
 * The P4 is little-endian and so is every host that runs the harness, which is
 * precisely why an endianness assumption here would never be caught. */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v      & 0xFF);
    p[1] = (uint8_t)(v >> 8 & 0xFF);
    p[2] = (uint8_t)(v >> 16 & 0xFF);
    p[3] = (uint8_t)(v >> 24 & 0xFF);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v      & 0xFF);
    p[1] = (uint8_t)(v >> 8 & 0xFF);
}

size_t wspr_wav_header(uint8_t *hdr, uint32_t nsamples, uint32_t rate)
{
    if (!hdr || nsamples == 0 || rate == 0) return 0;

    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint16_t block_align = (uint16_t)(channels * (bits / 8));      /* 2 */
    const uint32_t byte_rate   = rate * block_align;                     /* 24000 */
    const uint32_t data_bytes  = nsamples * block_align;

    memcpy(hdr + 0, "RIFF", 4);
    /* RIFF size counts everything AFTER this field: the 4-byte "WAVE" tag, the
     * 24-byte fmt chunk (8 header + 16 body) and the 8-byte data chunk header,
     * i.e. 36, plus the payload. Writing data_bytes + 44 here is the classic
     * off-by-eight and the harness pins it against a real file. */
    put_u32(hdr + 4, 36u + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);

    memcpy(hdr + 12, "fmt ", 4);
    put_u32(hdr + 16, 16u);              /* PCM fmt chunk body size */
    put_u16(hdr + 20, 1u);               /* WAVE_FORMAT_PCM */
    put_u16(hdr + 22, channels);
    put_u32(hdr + 24, rate);
    put_u32(hdr + 28, byte_rate);
    put_u16(hdr + 32, block_align);
    put_u16(hdr + 34, bits);

    memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, data_bytes);

    return WSPR_WAV_HDR_BYTES;
}

size_t wspr_wav_filename(char *out, size_t n, int64_t utc_epoch)
{
    if (!out || n == 0) return 0;
    time_t t = (time_t)utc_epoch;
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    /* YYMMDD_HHMM - WSJT-X's own convention, so wsprd needs no rename step.
     * Seconds are deliberately absent: a WSPR window always starts on an even
     * minute, so they would always be "00" and only make the name longer. */
    int len = snprintf(out, n, "%02d%02d%02d_%02d%02d.wav",
                       (tmv.tm_year + 1900) % 100, tmv.tm_mon + 1, tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min);
    if (len < 0) { out[0] = '\0'; return 0; }
    return (size_t)len < n ? (size_t)len : n - 1;
}
