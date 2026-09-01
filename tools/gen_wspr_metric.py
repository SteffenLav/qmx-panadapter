#!/usr/bin/env python3
"""Generate the WSPR soft-decision Fano metric table by simulation.

    python tools/gen_wspr_metric.py > main/wspr_metric_table.h

WHY THIS IS GENERATED AND NOT COPIED
    WSJT-X ships an equivalent table (metric_tables.h) under GPL. This project
    is MIT, so that table cannot be vendored. It also does not have to be: the
    table is not a magic constant, it is the measured statistics of a
    non-coherent 2-FSK soft symbol, and simulating them takes a few seconds.
    Generating it here has a second, larger benefit - the table is fitted to
    OUR normalisation, so it cannot be subtly mismatched to the decoder that
    uses it. A per-capture soft metric was tried once before and regressed the
    real WAV badly (docs/wspr-phase1-status.md); a table fitted to the wrong
    normalisation is exactly how that happens.

WHAT THE NUMBER IN EACH SLOT MEANS
    The Fano metric for a hypothesised bit b given a received soft symbol y is
    the log-likelihood ratio against the unconditional distribution:

        m(y, b) = log2( P(y | b) / P(y) ),  P(y) = (P(y|0) + P(y|1)) / 2

    which is at most 1 bit when the symbol is certain, and negative when the
    symbol argues against b. The decoder then subtracts a fixed BIAS per
    symbol - the rate term - so that a random path's metric drifts DOWN and
    the search can tell a good path from a lucky one.

THE SIMULATION MIRRORS THE DECODER EXACTLY, WHICH IS THE POINT
    One WSPR transmission is 162 symbols. Each is 4-FSK, but the sync bit is
    known, so the data bit is a 2-way non-coherent choice between two tone
    powers. The decoder forms  y = amplitude(tone_for_1) - amplitude(tone_for_0),
    normalises the 162 values of one transmission by their own standard
    deviation, scales by SYMFAC and clips to a byte. So this simulates whole
    162-symbol blocks and normalises each block the same way - NOT independent
    symbols, because the normaliser is a property of the block.
"""
import math, random, sys

NSYM   = 162
SYMFAC = 50.0          # must match WSPR_SOFT_SYMFAC in wspr_decode.c
BLOCKS = 60000
SEED   = 20260827

# The Es/No the table is fitted at. This is a REAL tuning knob, not a detail:
# a table fitted high is overconfident about a marginal symbol and lets the
# Fano search charge off down a wrong path, one fitted low throws away
# information the strong symbols carry. wsprd uses its 6 dB table but then
# feeds it symbols scaled by 50/72.7, i.e. it runs an effectively FLATTER
# table than the one it fitted - a historical quirk we do not have to inherit,
# because this generator fits the table to the same normalisation the decoder
# applies. Swept against the four reference WAVs; see docs/wspr-phase3-sensitivity.md.
ESNO_DB = float(sys.argv[sys.argv.index('--esno') + 1]) if '--esno' in sys.argv else 4.0


def soft_block(rnd, amp):
    """One 162-symbol transmission -> (byte value, true bit) per symbol.

    Non-coherent 2-FSK: the branch carrying the bit sees signal+noise (Rice),
    the other sees noise alone (Rayleigh). Unit noise variance per complex
    dimension, so `amp` alone sets Es/No.
    """
    bits = [rnd.getrandbits(1) for _ in range(NSYM)]
    y = []
    for b in bits:
        # signal branch
        si = amp + rnd.gauss(0.0, 1.0)
        sq = rnd.gauss(0.0, 1.0)
        ps = math.hypot(si, sq)
        # noise-only branch
        ni = rnd.gauss(0.0, 1.0)
        nq = rnd.gauss(0.0, 1.0)
        pn = math.hypot(ni, nq)
        y.append(ps - pn if b else pn - ps)
    mean = sum(y) / NSYM
    var  = sum(v * v for v in y) / NSYM - mean * mean
    sd   = math.sqrt(var) if var > 1e-12 else 1e-6
    out = []
    for v, b in zip(y, bits):
        s = SYMFAC * v / sd
        s = 127.0 if s > 127.0 else (-128.0 if s < -128.0 else s)
        out.append((int(round(s)) + 128, b))
    return out


def main():
    # Es/No per symbol -> signal amplitude, with unit variance per quadrature.
    # Es/N0 = amp^2 / (2 * sigma^2), sigma = 1  =>  amp = sqrt(2 * 10^(dB/10))
    amp = math.sqrt(2.0 * 10 ** (ESNO_DB / 10.0))
    rnd = random.Random(SEED)
    h0 = [0.0] * 256
    h1 = [0.0] * 256
    for _ in range(BLOCKS):
        for idx, b in soft_block(rnd, amp):
            if idx < 0: idx = 0
            if idx > 255: idx = 255
            (h1 if b else h0)[idx] += 1.0

    n0 = sum(h0); n1 = sum(h1)
    # Laplace floor: an empty bin must not become -inf, and the tails ARE the
    # part the weakest signals live in.
    FLOOR = 0.5
    p0 = [(v + FLOOR) / (n0 + 256 * FLOOR) for v in h0]
    p1 = [(v + FLOOR) / (n1 + 256 * FLOOR) for v in h1]

    # m0[i] is the metric for hypothesised bit 0 at symbol value i. The channel
    # is symmetric, so m1[i] == m0[255-i] and only one table need be stored -
    # which is also how it stays a 512-byte const in flash rather than 1 KB.
    m0 = []
    for i in range(256):
        m0.append(math.log2(2.0 * p0[i] / (p0[i] + p1[i])))

    # The true metric is monotone in the soft value - a symbol further towards
    # "0" cannot argue LESS for bit 0. Simulation noise in the tails breaks
    # that (the extreme bins see almost no counts of the wrong class), and a
    # non-monotone tail can only mislead the search. Enforce it, and floor the
    # tail: no single symbol should be able to veto a path outright.
    FLOOR_BITS = -8.0
    run = 1.0
    for i in range(256):
        run = min(run, m0[i])
        m0[i] = max(run, FLOOR_BITS)

    # Emit as scaled integers so the decoder does no float work per branch.
    SCALE = 1000
    q = [int(round(SCALE * v)) for v in m0]

    w = sys.stdout.write
    w("/* GENERATED by tools/gen_wspr_metric.py - do not edit by hand.\n")
    w(" *\n")
    w(" * Fano branch metric for a non-coherent 2-FSK soft symbol, fitted by\n")
    w(" * simulation at Es/No = %.1f dB with the decoder's own normalisation\n" % ESNO_DB)
    w(" * (SYMFAC = %g over one 162-symbol transmission). Run the generator to\n" % SYMFAC)
    w(" * reproduce; see its header for what the numbers mean and why the table\n")
    w(" * is ours rather than WSJT-X's (licence, and normalisation matching).\n")
    w(" *\n")
    w(" * Units: log2-likelihood x %d. Index is the received soft byte.\n" % SCALE)
    w(" * wspr_metric0[i] is the metric for bit 0; bit 1 is wspr_metric0[255-i],\n")
    w(" * which the channel's symmetry guarantees.\n")
    w(" */\n")
    w("#ifndef WSPR_METRIC_TABLE_H\n#define WSPR_METRIC_TABLE_H\n#include <stdint.h>\n\n")
    w("#define WSPR_METRIC_SCALE %d\n" % SCALE)
    w("#define WSPR_METRIC_ESNO_DB %.1ff\n\n" % ESNO_DB)
    w("static const int16_t wspr_metric0[256] = {\n")
    for r in range(0, 256, 8):
        w("    " + ", ".join("%6d" % v for v in q[r:r + 8]) + ",\n")
    w("};\n\n#endif /* WSPR_METRIC_TABLE_H */\n")


main()
