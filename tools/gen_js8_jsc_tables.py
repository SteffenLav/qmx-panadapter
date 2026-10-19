#!/usr/bin/env python3
"""Emit JS8's free-text coding tables as C, straight from JS8Call's source.

    python tools/gen_js8_jsc_tables.py <jsc_map.cpp> <varicode.cpp> > \
        components/ft8_lib/ft8/js8_jsc_tables.c

⛔ NOT HAND-TRANSCRIBED, for the same reason as tools/gen_js8_ldpc_tables.py:
a wrong entry in a coding table is indistinguishable from a decode bug. Both
read as "the text comes out as rubbish". This parses the source, so the only
thing a human writes is the path to it.

LICENCE. The tables are JS8Call's (GPL-3, Jordan Sherer KN4CRD). This project
relicensed from MIT to GPL-3 on 2026-10-09 precisely so they could be used,
and the generated file carries the attribution. Under MIT they were out of
reach - not because the protocol is closed, but because a 262144-word
dictionary has to be matched bit for bit and cannot be reimplemented.

WHY THE WORD LIST IS TRUNCATED, AND WHERE. JS8's (s,c)-dense coding with
b=4, s=7, c=9 puts the index in bands:

    k  index range          words    bits/word   words per 70-bit frame
    0        0..6               7        5            14
    1        7..69             63        8             8
    2       70..636           567       12             5
    3      637..5739         5103       16             4
    4     5740..51666       45927       20             3
    5    51667..262143      210477      24             2

The full list is 2893 KB against about 2.4 MB of free app partition - it does
not fit, licence or no licence. Cutting at 51667 keeps EVERY index reachable
in 20 bits or fewer, which is a boundary the coding itself draws rather than a
number someone picked. The 210477 words above it cost 24 bits each, so only
two of them fit in a frame at all, and the list is frequency-ordered (index 0
is "e"), so the tail that goes is the rare tail.

STORAGE. A NUL-separated blob in index order, plus a SPARSE offset table every
JS8_JSC_STRIDE words. A full offset table would cost 202 KB for the offsets
alone - two thirds as much as the words. With a stride of 64 it costs 3 KB and
a lookup skips at most 63 short strings, which is nothing next to an LDPC
pass. Measured: 305.6 KB of words + 3.2 KB of index = 309 KB.
"""
import re
import sys

STRIDE = 64
# base[5] from the dense coding - the first index needing 24 bits.
WORDS = 51667


def read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


C_ESCAPE = re.compile(r'\\(x[0-9a-fA-F]{1,2}|[0-7]{1,3}|.)')
_SIMPLE = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0',
           '"': '"', "'": "'", '\\': '\\'}


def unescape_c(lit):
    """A C string-literal body to the characters it denotes.

    Needed because 32 of the entries are a single high byte written as an
    escape. Reading the literal text would have put a backslash and an x into
    the word list."""
    def sub(m):
        g = m.group(1)
        if g[0] in 'xX':
            return chr(int(g[1:], 16))
        if g[0] in '01234567' and len(g) > 1:
            return chr(int(g, 8))
        return _SIMPLE.get(g, g)

    return C_ESCAPE.sub(sub, lit)


# /* ... */ COMMENTS SIT BETWEEN THE FIELDS of 32 entries, like
#     {"\xa1" /* <that char> - "BO60" */, 1, 10704},
# and a pattern allowing only whitespace between fields skipped every one of
# them - 32 words lost, which the index check below caught on the first run.
# Stripped first rather than matched around, because the comments themselves
# contain quoted text that would confuse the field pattern.
COMMENT = re.compile(r'/\*.*?\*/', re.S)


def parse_jsc_map(src):
    """The `{ "word", size, index }` tuples of JSC::map, in file order.

    File order IS index order: JSC::decompress() indexes map[] directly, so a
    sort here would silently shift every word by one place."""
    src = COMMENT.sub(' ', src)
    rows = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', src)
    if len(rows) < WORDS:
        raise SystemExit('jsc_map.cpp gave %d entries, need at least %d'
                         % (len(rows), WORDS))
    out = []
    for i, (w, _size, idx) in enumerate(rows[:WORDS]):
        # The third field is the entry's own index. Checking it against the
        # position is the whole guard against a parse that silently skipped a
        # line - which would shift every later word and decode as fluent
        # nonsense. It earned its place immediately: the first run stopped at
        # entry 10704, which declared 10736.
        if int(idx) != i:
            raise SystemExit('jsc_map entry %d declares index %s - the parse '
                             'lost a line, and every word after it would be '
                             'wrong' % (i, idx))
        # Unescape, then treat the high bytes as Latin-1 and re-encode UTF-8
        # at blob-build time. JSC::decompress() says "map is in latin1 format,
        # not utf-8", and a lone 0xA1 is invalid UTF-8: it would draw as
        # garbage on an LVGL label and break the JSON of /api/status.
        out.append(unescape_c(w))
    return out


def parse_hufftable(src):
    """varicode.cpp's `hufftable`: character -> bit string."""
    m = re.search(r'QMap<QString,\s*QString>\s+hufftable\s*=\s*\{(.*?)\n\};',
                  src, re.S)
    if not m:
        raise SystemExit('no hufftable in varicode.cpp')
    body = m.group(1)
    rows = re.findall(r'\{\s*(?:"((?:[^"\\]|\\.)*)"|\'(.)\')\s*,\s*"([01]+)"\s*\}',
                      body)
    out = []
    for a, b, code in rows:
        ch = a if a else b
        # The table's key for the double quote is written "\"" in the source.
        if ch == '\\"':
            ch = '"'
        elif ch == '\\\\':
            ch = '\\'
        if len(ch) != 1:
            raise SystemExit('hufftable key %r is not one character' % ch)
        out.append((ch, code))
    if not out:
        raise SystemExit('hufftable parsed to nothing')
    # A prefix-free code is the one property the decoder relies on; if two
    # entries collide or one prefixes another the decode silently desyncs.
    codes = [c for _ch, c in out]
    if len(set(codes)) != len(codes):
        raise SystemExit('hufftable has duplicate codes')
    for i, a in enumerate(codes):
        for b in codes[i + 1:]:
            if a.startswith(b) or b.startswith(a):
                raise SystemExit('hufftable is not prefix-free: %r vs %r' % (a, b))
    return out


def c_bytes(s):
    """A C string-literal body. Anything outside printable ASCII goes out as an
    octal escape, so the generated file is pure ASCII and cannot be mangled by
    an editor's encoding guess."""

    out = []
    for b in (s if isinstance(s, bytes) else s.encode('utf-8')):
        c = chr(b)
        if c == '"':
            out.append('\\"')
        elif c == '\\':
            out.append('\\\\')
        elif 0x20 <= b < 0x7f:
            out.append(c)
        else:
            out.append('\\%03o' % b)
    return ''.join(out)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    words = parse_jsc_map(read(sys.argv[1]))
    huff = parse_hufftable(read(sys.argv[2]))

    blob = bytearray()
    offsets = []
    for i, w in enumerate(words):
        if i % STRIDE == 0:
            offsets.append(len(blob))
        blob += w.encode('utf-8') + b'\x00'

    w = sys.stdout.write
    w('/* GENERATED by tools/gen_js8_jsc_tables.py - DO NOT HAND-EDIT.\n'
      ' *\n'
      ' * JS8 free-text coding tables, extracted from JS8Call 2.3.1\n'
      ' * (tag v2.3.1, fd721e8b67ee): jsc_map.cpp and varicode.cpp.\n'
      ' * (C) 2018 Jordan Sherer <kn4crd@gmail.com>, GPL-3.\n'
      ' *\n'
      ' * This project is GPL-3 (relicensed from MIT on 2026-10-09 so that\n'
      ' * these tables could be used at all). Regenerate rather than edit:\n'
      ' * a wrong entry here reads exactly like a decoder bug.\n'
      ' *\n'
      ' * The word list is TRUNCATED at %d entries - every index reachable in\n'
      ' * 20 bits or fewer. See the generator for why that boundary and not\n'
      ' * another. Words at or above it decode as a placeholder.\n'
      ' */\n' % WORDS)
    w('#include "js8_jsc.h"\n\n')

    w('const uint32_t kJS8_JSC_words  = %d;\n' % len(words))
    w('const uint32_t kJS8_JSC_stride = %d;\n' % STRIDE)
    w('const uint32_t kJS8_JSC_blob_len = %d;\n\n' % len(blob))

    w('/* Byte offset of word (i * %d) in the blob. */\n' % STRIDE)
    w('const uint32_t kJS8_JSC_index[%d] = {\n' % len(offsets))
    for i in range(0, len(offsets), 8):
        w('    ' + ' '.join('%8d,' % v for v in offsets[i:i + 8]) + '\n')
    w('};\n\n')

    w('/* NUL-separated, in index order. */\n')
    w('const char kJS8_JSC_blob[%d] =\n' % len(blob))
    text = c_bytes(bytes(blob))
    # Chunked so no single literal is absurd; the compiler concatenates them.
    CH = 110
    i = 0
    while i < len(text):
        j = min(i + CH, len(text))
        # Never split an escape sequence across two literals.
        while j > i and j < len(text) and text[j - 1] == '\\':
            j -= 1
        k = j
        while k > i:
            tail = text[i:k]
            m = re.search(r'\\[0-7]{0,2}$|\\$', tail)
            if not m:
                break
            k -= 1
        j = k if k > i else j
        w('    "%s"\n' % text[i:j])
        i = j
    w(';\n\n')

    w('/* varicode.cpp hufftable: the per-character alphabet for [10x]\n'
      ' * frames. Prefix-free, checked by the generator. */\n')
    w('const js8_huff_entry_t kJS8_Huff[%d] = {\n' % len(huff))
    for ch, code in huff:
        lit = c_bytes(ch)
        w('    { "%s", "%s" },\n' % (lit, code))
    w('};\n')
    w('const uint32_t kJS8_Huff_count = %d;\n' % len(huff))


if __name__ == '__main__':
    main()
