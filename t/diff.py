#!/usr/bin/env python3
"""Differential oracles for xmlc, all of them against Python's standard library.

SPDX-License-Identifier: GPL-3.0-or-later

Modes:
    utf8 BIN      xmlc -C against Python's own UTF-8 decoder
"""

import itertools
import re
import os
import subprocess
import sys

PASS = [0]
FAIL = [0]


def ok(msg):
    PASS[0] += 1
    print("ok - " + msg)


def nok(msg):
    FAIL[0] += 1
    print("not ok - " + msg)


def run(bin_, args, data):
    r = subprocess.run([bin_] + args, input=data, capture_output=True)
    return r.returncode, r.stdout, r.stderr


# ------------------------------------------------------------------- utf8

# Boundary bytes.  Every lead-byte class, both sides of every overlong and
# surrogate edge, and the bytes that are not UTF-8 lead bytes at all.
LEADS = [0x00, 0x7F, 0x80, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC,
         0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xF7, 0xF8, 0xFE,
         0xFF]
TAILS = [0x00, 0x20, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0,
         0xFF]


def utf8_cases():
    """Byte strings whose UTF-8 validity is the thing being compared."""
    for b in range(256):                       # every single byte
        yield bytes([b])
    for a, b in itertools.product(LEADS, TAILS):
        yield bytes([a, b])
    for a, b, c in itertools.product([0xE0, 0xED, 0xEF, 0xF0, 0xF4],
                                     [0x80, 0x9F, 0xA0, 0xBF],
                                     [0x80, 0xBF, 0xC0]):
        yield bytes([a, b, c])
    for a, b, c, d in itertools.product([0xF0, 0xF4],
                                        [0x80, 0x8F, 0x90, 0xBF],
                                        [0x80, 0xBF], [0x80, 0xBF]):
        yield bytes([a, b, c, d])
    # The same sequences with a legal character in front, so the error is not
    # always at offset zero.
    for s in (b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe0\x80\x80",
              b"\xc2", b"\xe2\x82", b"\xf0\x9f\x98"):
        yield b"a" + s
        yield b"a" + s + b"b"


BYTEAT = re.compile(rb"\(byte (\d+)\)")


def mode_utf8(bin_):
    """Python decides what is UTF-8, and at which byte it stops being UTF-8.

    The first version of this compared verdicts only, and eight of 638
    sequences "disagreed".  All eight began with a NUL: xmlc stops at offset 0
    because NUL is not a legal [2] Char, never reaches the ill-formed byte at
    offset 1, and reports the Char error -- while Python decodes the whole
    string and reports the UTF-8 error.  Neither was wrong.  The harness was:
    xmlc -C answers two questions at once and only the first has an
    independent authority in the standard library.

    So the comparison is on offsets, which makes it stronger rather than
    weaker.  Python gives the offset where decoding fails; xmlc prints the
    offset where it stopped and which of the two rules stopped it:

      * Python decodes everything -> xmlc must not report a UTF-8 error.
      * Python fails at byte k    -> xmlc must stop at or before k, and if it
                                     stops exactly at k the reason must be the
                                     UTF-8 one.
    """
    bad = []
    n = 0
    for data in utf8_cases():
        n += 1
        try:
            data.decode("utf-8")
            pyfail = None
        except UnicodeDecodeError as e:
            pyfail = e.start
        st, out, err = run(bin_, ["-C"], data)
        if st not in (0, 1):
            bad.append((data, "exit %d" % st))
            continue
        isutf8err = b"malformed UTF-8" in err
        m = BYTEAT.search(err)
        off = int(m.group(1)) if m else None
        if pyfail is None:
            if isutf8err:
                bad.append((data, "python decodes it, xmlc calls it bad UTF-8"))
        elif off is None:
            bad.append((data, "python fails at %d, xmlc accepted" % pyfail))
        elif off > pyfail:
            bad.append((data, "python fails at %d, xmlc only at %d"
                        % (pyfail, off)))
        elif off == pyfail and not isutf8err:
            bad.append((data, "both stop at %d but for different reasons: %s"
                        % (off, err.decode(errors="replace").strip())))
    if bad:
        nok("UTF-8 decoder agrees with Python over %d sequences (%d differ)"
            % (n, len(bad)))
        for d, why in bad[:20]:
            print("    %-24s %s" % (d.hex(), why))
    else:
        ok("UTF-8 decoder agrees with Python, offset for offset, over %d "
           "boundary sequences" % n)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mode, bin_ = sys.argv[1], sys.argv[2]
    fn = globals().get("mode_" + mode)
    if not fn:
        print("unknown mode %s" % mode, file=sys.stderr)
        return 2
    fn(bin_, *sys.argv[3:])
    return 1 if FAIL[0] else 0


if __name__ == "__main__":
    sys.exit(main())
