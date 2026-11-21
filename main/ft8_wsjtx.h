/* ft8_wsjtx - WS 3.2.1's FT8 decoder (lib/ft8_decode.f90 and lib/ft8/),
 * ported to C routine by routine. Algorithms and constants are the Fortran's;
 * each function names the routine it ports. GPL-3, (c) K1JT, K9AN, G4WJS,
 * DG2YCB et al.; port 2026-11-21.
 *
 * Input is one 15 s slot at 12000 S/s as int16, exactly what jt9 reads from a
 * WAV: the absolute level matters, because ft8b's false-decode screen compares
 * a signal power against the spectrum baseline in absolute units.
 */
#pragma once
#include <stdint.h>
#include "ft8/message.h"

#define WSX_NMAX (15 * 12000)

typedef struct {
    int nfa, nfb;        /* search band, Hz (jt9: 200, 4000) */
    int nfqso, nftx;     /* rx / tx audio frequency (jt9: 1500, 1500) */
    int ndepth;          /* 1..3 (jt9 -d) */
    int lapon;           /* AP decoding on (jt9: on) */
    int napwid;          /* AP width, Hz (jt9: 75) */
    int nqso_progress;   /* 0..5 (jt9: 0) */
    int nutc;            /* slot id: jseq = (nutc/5)%2 picks the even/odd a7
                          * table. jt9: the last 6 digits of the WAV name. */
    char mycall[13];     /* jt9 default K1ABC */
    char hiscall[13];    /* jt9 default W9XYZ */
    char hisgrid[7];     /* jt9 default EN37; a8 needs >= 4 chars */
    /* Callsign hash table for unpack (jt9: packjt77's, filled by every
     * decode in the process). NULL prints every hashed call as <...>. */
    ftx_callsign_hash_interface_t* hash_if;
} wsx_params_t;

/* One decode. nap = 0 for a plain decode, 1..6 for an a priori one, 7 for
 * an a7 (previous-cycle) one, 8 for an a8 (list decode at the RX frequency) (jt9 prints these with a trailing "aN"). */
typedef void (*wsx_callback_t)(void* ctx, float sync, int snr, float dt, float freq,
                               const char* msg, int nap, float qual);

void wsx_default_params(wsx_params_t* p);

/* jt9's disk-mode sequence for one slot: decode at nzhsym 41 and 47 on the
 * truncated slot, then at 50 on the whole of it (jt9.f90, mode 8). */
void wsx_decode_slot(const int16_t* iwave, const wsx_params_t* p,
                     wsx_callback_t cb, void* ctx);

/* The same, split where WSJT-X splits it in time. early() is call 41 and
 * reads only iwave[0 .. 41*3456 = 141696) (11.8 s); late() is calls 47 and
 * 50 on the whole slot. late() must follow early() for the SAME slot, with
 * nothing in between: the decoder state is one static. Not reentrant. */
#define WSX_EARLY_SAMPLES (41 * 3456)
void wsx_decode_early(const int16_t* iwave, const wsx_params_t* p,
                      wsx_callback_t cb, void* ctx);
void wsx_decode_late(const int16_t* iwave, const wsx_params_t* p,
                     wsx_callback_t cb, void* ctx);

/* -DWSX_PROF builds only: per-stage times to stderr. */
void wsx_prof_print(void);

/* Single pieces, for measurement on another decoder's candidates (see the
 * pieces section in ft8_wsjtx.c). xdt is WSJT-X's DT (signal start - 0.5 s);
 * npasses limits ft8b's metric passes (1..5, 0 = all), maxosd -1 = BP only. */
void wsx_piece_begin(const int16_t* iwave, int npasses, int maxosd);
int  wsx_piece_try(float f1, float xdt, int imetric, char* msg, float* f_out, float* dt_out);
int  wsx_piece_try_ex(float f1, float xdt, int imetric, char* msg, float* f_out, float* dt_out,
                      float* snr_out, int* itone_out /* [79] */);
void wsx_piece_end(void);
/* Fill-in-place variant, for a caller that already holds the slot as floats:
 * write up to WSX_NMAX samples into wsx_piece_audio() (NULL = no memory),
 * then wsx_piece_begin_audio(n, ...) normalises them exactly as the corpus is
 * (peak -> int16 full scale). wsx_piece_release() frees every buffer and FFT
 * plan; the next wsx_piece_audio() allocates them again. */
float* wsx_piece_audio(void);
int    wsx_piece_begin_audio(int n, int npasses, int maxosd);
void   wsx_piece_release(void);
/* OSD depth (decode174_91 norder) for the piece path; ft8b uses 2. Reset by
 * wsx_piece_end(). */
void   wsx_piece_set_ndeep(int ndeep);
/* AP type 1 (CQ) in the piece path; off by default. Call after begin. */
void   wsx_piece_set_ap_cq(int on);
