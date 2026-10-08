#!/usr/bin/env python3
"""Check js8_encode() against JS8Call's own genjs8.f90, compiled with gfortran.

WHY. Everything else in the JS8 work so far agrees only with itself: the
decode harness decodes signals this code produced, and the message layer was
checked against values computed by hand from the published formulas. The tone
path is the one part that CAN be put in front of an outside arbiter without a
radio, because genjs8.f90 does its own message packing - it takes 12 characters
of a 68-character alphabet, not a varicode frame - so none of JS8Call's Qt C++
is needed to run it.

WHAT IT PROVES. For N pseudo-random 72-bit frames: our 79 tones are bit-exact
with the Fortran's. That covers the 6-bit word packing, the CRC buffer layout,
the transmission-type bits, encode174, the colorder permutation and the tone
layout (Costas at 0/36/72, data at 7..35 and 43..71, c0*4 + c1*2 + c2 with no
Gray map).

⛔ WHAT IT DOES NOT PROVE. The CRC-12 ENGINE. JS8Call's crc12.cpp is a
one-line call into boost::augmented_crc and Boost is not on this machine, so
the shim below feeds the Fortran OUR js8_crc12_raw(). If that engine is wrong
both sides are wrong together and this script still passes. The CRC value does
reach the tones, so a real JS8Call station remains the only arbiter for it -
and never a second Tab5.

Needs: gfortran and gcc on PATH (the WinLibs mingw64 toolchain has both), and
network access to raw.githubusercontent.com.

Usage:
  python tools/js8_tone_arbiter.py [count]        # default 64
  python tools/js8_tone_arbiter.py --src <dir>    # use an existing checkout
"""
import os
import subprocess
import sys
import tempfile
import urllib.request

TAG = 'v2.3.1'  # fd721e8b67ee
RAW = 'https://raw.githubusercontent.com/js8call/js8call/' + TAG + '/'
SOURCES = [
    'lib/crc.f90',
    'lib/js8/genjs8.f90',
    'lib/js8/js8_params.f90',
    'lib/ft8/encode174.f90',
    'lib/ft8/ldpc_174_87_params.f90',
]

CRC_SHIM = r'''
#include <stdint.h>
#include <stdbool.h>
/* Stands in for crc12.cpp's boost::augmented_crc<12,0xc06>. This is OUR
 * engine, so the CRC is the one link this arbiter cannot check. */
uint16_t js8_crc12_raw(const uint8_t* data, int nbits, uint16_t poly);
short crc12(const unsigned char* data, int length)
{
    return (short)js8_crc12_raw(data, length * 8, 0xC06u);
}
bool crc12_check(const unsigned char* data, int length)
{
    return crc12(data, length) == 0;
}
'''

ARB_MAIN = r'''
program arb
  character*22 msg, msgsent
  integer*1 msgbits(87)
  integer itone(79)
  integer i3bit, i, ios
  character*64 line
10 read(*,'(a)',iostat=ios) line
  if(ios.ne.0) stop
  msg = line(1:12)
  read(line(14:14),'(i1)') i3bit
  call genjs8(msg, 1, i3bit, msgsent, msgbits, itone)
  write(*,'(79(i1))') (itone(i), i=1,79)
  go to 10
end program arb
'''

OURS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "js8.h"

/* genjs8.f90 builds its 72 message bits from 12 six-bit alphabet indices, MSB
 * first - the same bit order js8_pack_payload() reads a frame in. So a frame
 * and a 12-character message are two spellings of the same 72 bits. */
static const char* ALPHABET =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";

int main(int argc, char** argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 64;
    unsigned seed = 20261008u;
    for (int t = 0; t < n; t++)
    {
        uint8_t frame[9];
        for (int i = 0; i < 9; i++)
        {
            seed = seed * 1103515245u + 12345u;
            frame[i] = (uint8_t)(seed >> 16);
        }
        seed = seed * 1103515245u + 12345u;
        uint8_t itype = (uint8_t)((seed >> 16) % 8);

        char msg[13];
        for (int w = 0; w < 12; w++)
        {
            int bit = w * 6, v = 0;
            for (int b = 0; b < 6; b++)
            {
                int ix = bit + b;
                v = (v << 1) | ((frame[ix / 8] >> (7 - (ix % 8))) & 1);
            }
            msg[w] = ALPHABET[v];
        }
        msg[12] = '\0';

        uint8_t tones[JS8_NN];
        js8_encode(frame, itype, tones);

        printf("%s %d ", msg, (int)itype);
        for (int i = 0; i < JS8_NN; i++) printf("%d", tones[i]);
        printf("\n");
    }
    return 0;
}
'''


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        raise SystemExit('command failed')
    return r.stdout


def main():
    args = [a for a in sys.argv[1:]]
    src_dir = None
    if '--src' in args:
        i = args.index('--src')
        src_dir = args[i + 1]
        del args[i:i + 2]
    count = int(args[0]) if args else 64

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ft8 = os.path.join(repo, 'components', 'ft8_lib', 'ft8')

    work = tempfile.mkdtemp(prefix='js8_arbiter_')
    for rel in SOURCES:
        name = os.path.basename(rel)
        dst = os.path.join(work, name)
        if src_dir:
            with open(os.path.join(src_dir, rel), 'rb') as f:
                data = f.read()
        else:
            data = urllib.request.urlopen(RAW + rel, timeout=30).read()
        with open(dst, 'wb') as f:
            f.write(data)

    for name, text in (('crc_shim.c', CRC_SHIM), ('arb_main.f90', ARB_MAIN),
                       ('ours.c', OURS)):
        with open(os.path.join(work, name), 'w') as f:
            f.write(text)

    codec = [os.path.join(ft8, 'js8_codec.c'), os.path.join(ft8, 'js8_tables.c')]

    run(['gfortran', '-O2', '-o', os.path.join(work, 'arb.exe'),
         'crc.f90', 'arb_main.f90', 'genjs8.f90', 'encode174.f90', 'crc_shim.c']
        + codec + ['-I' + ft8], cwd=work)
    run(['gcc', '-O2', '-Wall', '-o', os.path.join(work, 'ours.exe'),
         'ours.c'] + codec + ['-I' + ft8], cwd=work)

    ours = run([os.path.join(work, 'ours.exe'), str(count)]).split('\n')
    ours = [l.split() for l in ours if l.strip()]
    feed = ''.join('%s %s\n' % (m, t) for m, t, _ in ours)

    r = subprocess.run([os.path.join(work, 'arb.exe')], input=feed,
                       capture_output=True, text=True)
    theirs = [l.strip() for l in r.stdout.split('\n') if l.strip()]

    if len(theirs) != len(ours):
        raise SystemExit('arbiter returned %d lines for %d frames'
                         % (len(theirs), len(ours)))

    bad = 0
    for (msg, itype, mine), yours in zip(ours, theirs):
        if mine != yours:
            bad += 1
            print('MISMATCH msg=%s itype=%s' % (msg, itype))
            print('  ours %s' % mine)
            print('  js8c %s' % yours)

    print('%d of %d tone sequences match genjs8.f90' % (len(ours) - bad, len(ours)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
