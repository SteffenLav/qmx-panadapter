#pragma once
/* Only what js8_submode.c needs: the protocol enum and its human label. The
 * real table lives in components/ft8_lib/ft8/constants.c and is NOT linked
 * here - this harness tests the submode table, not the protocol one. */
typedef enum {
    FTX_PROTOCOL_FT4 = 0, FTX_PROTOCOL_FT8,
    FTX_PROTOCOL_JS8, FTX_PROTOCOL_JS8_FAST,
    FTX_PROTOCOL_JS8_TURBO, FTX_PROTOCOL_JS8_SLOW,
} ftx_protocol_t;
const char *ftx_protocol_label(ftx_protocol_t p);
