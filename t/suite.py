#!/usr/bin/env python3
"""Score xmlc against the W3C XML Conformance Test Suite.

SPDX-License-Identifier: GPL-3.0-or-later

The suite is not a flat pile of files, it is a catalogue: xmlconf/xmlconf.xml
lists 2585 <TEST> elements, each carrying the W3C's own classification of what
the case is for.  That classification -- not our reading of the file -- is what
decides whether a case is in scope for a non-validating XML 1.0 Fifth Edition
parser that does not fetch external entities and only reads UTF-8.

    TYPE            valid | invalid | not-wf | error
    ENTITIES        none | general | parameter | both
    VERSION         1.0 | 1.1 | absent
    EDITION         "5" | "1 2 3 4" | absent
    RECOMMENDATION  XML1.0 | XML1.1 | XML1.0-errata2e/3e/4e | NS1.0
                    | NS1.0-errata1e | NS1.1

Writing the scope filter in those terms is the whole point.  "Out of scope" is
then a claim the suite makes about its own test, not an excuse we invented for
a failure.

The catalogue is itself an XML document that pulls its sections in through
external parameter entities, so it is parsed with Python's expat -- which is
also oracle number two.

Modes:
    score  ROOT BIN     accept/reject over the catalogue, per TYPE

Planned, and refused rather than listed as working:
    canon  ROOT BIN     canonical form against the suite's own out/*.xml
                        -- needs the canonical dump, PLAN item 28
    diff   ROOT BIN     differential against expat: verdict, then events
                        -- PLAN item 36

This docstring is what main() prints when it is called with too few
arguments, so it is a usage message and not a plan. It listed all three
modes as if they worked.
"""

import collections
import os
import re
import subprocess
import sys
from xml.parsers import expat

# ---------------------------------------------------------------- catalogue

# Inherited down the TESTCASES nesting; a TEST's own attributes win.
INHERIT = ("PROFILE", "EDITION", "RECOMMENDATION", "NAMESPACE", "VERSION",
           "ENTITIES")


def load(root):
    """Parse xmlconf.xml, following external entity references, in order."""
    tests = []

    def hook(p, path, stack):
        def start(name, at):
            cur = dict(stack[-1])
            if "xml:base" in at:
                cur["base"] = os.path.join(cur.get("base", ""), at["xml:base"])
            for k in INHERIT:
                if k in at:
                    cur[k] = at[k]
            stack.append(cur)
            if name == "TEST":
                t = dict(cur)
                t.update(at)
                t["PATH"] = os.path.normpath(
                    os.path.join(root, cur.get("base", ""), at["URI"]))
                t["OUT"] = os.path.normpath(
                    os.path.join(root, cur.get("base", ""),
                                 at["OUTPUT"])) if "OUTPUT" in at else None
                tests.append(t)

        def ext(ctx, base, sysid, pub):
            sub = os.path.normpath(os.path.join(os.path.dirname(path), sysid))
            if not os.path.exists(sub):
                return 1
            sp = p.ExternalEntityParserCreate(ctx)
            hook(sp, sub, stack)
            with open(sub, "rb") as f:
                sp.ParseFile(f)
            return 1

        p.StartElementHandler = start
        p.EndElementHandler = lambda n: stack.pop()
        p.ExternalEntityRefHandler = ext

    top = os.path.join(root, "xmlconf.xml")
    p = expat.ParserCreate()
    p.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_ALWAYS)
    hook(p, top, [{"base": ""}])
    with open(top, "rb") as f:
        p.ParseFile(f)
    return tests


# ------------------------------------------------------------------- scope

ENCNAME = re.compile(rb'encoding\s*=\s*["\']([A-Za-z][A-Za-z0-9._-]*)["\']')
# The two encodings xmlc reads.  US-ASCII is a subset of UTF-8, so it is the
# same decoder; everything else is a named refusal, not a silent misread.
CAN_DECODE = {b"utf-8", b"us-ascii", b"ascii"}

OOS_REASON = {
    "xml11": "VERSION=1.1 or RECOMMENDATION=XML1.1/NS1.1: xmlc is 1.0 only",
    "namespaces": "RECOMMENDATION=NS*: namespace well-formedness is a "
                  "separate specification, and xmlc checks XML 1.0",
    "edition1-4": "EDITION excludes 5: superseded name or char rules",
    "extentity": "ENTITIES!=none: needs an external entity resolved",
    "encoding": "UTF-16, or a declared encoding xmlc refuses by design",
    "missing": "file is not in the archive",
}


def sniff(path):
    """Return (looks_utf16, declared_encoding_or_None, valid_utf8)."""
    try:
        with open(path, "rb") as f:
            b = f.read()
    except OSError:
        return (False, None, True)
    u16 = b[:2] in (b"\xfe\xff", b"\xff\xfe") or b[:2] in (b"\x00<", b"<\x00")
    m = ENCNAME.search(b[:200])
    try:
        b.decode("utf-8")
        ok = True
    except UnicodeDecodeError:
        ok = False
    return (u16, m.group(1).lower() if m else None, ok)


def classify(t):
    """None if xmlc is required to get this right, else the reason it is not."""
    rec = t.get("RECOMMENDATION", "")
    if t.get("VERSION") == "1.1" or rec in ("XML1.1", "NS1.1"):
        return "xml11"
    # Namespaces in XML is a different recommendation from XML 1.0, and a
    # document can be perfectly well-formed XML while breaking every rule in
    # it -- "<a:b/>" with no binding for a: is the standard example. xmlc
    # checks XML 1.0 well-formedness and says so, so judging these would be
    # answering a question it was not asked.
    #
    # NS1.1 was already excluded here, by this same reasoning, from the other
    # direction. The catalogue marks these RECOMMENDATION=NS1.0 itself, which
    # is the standard this module sets for itself at the top: out of scope has
    # to be a claim the suite makes about its own test, not an excuse invented
    # for a failure. They became visible when the XML declaration landed and
    # these documents started parsing far enough to be judged.
    #
    # This tested RECOMMENDATION == "NS1.0" exactly, and the catalogue also
    # uses "NS1.0-errata1e" for three cases. Those three went through the
    # filter and were scored, so an XML 1.0 parser was being marked wrong for
    # not implementing xmlns. The prefix is the claim the suite is making.
    if rec.startswith("NS"):
        return "namespaces"
    ed = t.get("EDITION")
    if ed and "5" not in ed.split():
        return "edition1-4"
    if not os.path.exists(t["PATH"]):
        return "missing"
    if t.get("ENTITIES", "none") != "none":
        return "extentity"
    u16, enc, utf8 = sniff(t["PATH"])
    if u16 or (enc is not None and enc not in CAN_DECODE) or not utf8:
        return "encoding"
    return None


# A non-validating parser must accept `invalid`: those documents are
# well-formed and only break a DTD constraint we do not check.  `error`
# cases are optional-error, so either verdict conforms and we only record
# which one we gave.
WANT = {"valid": 0, "invalid": 0, "not-wf": 1, "error": None}


# -------------------------------------------------------------------- runs

def run(bin_, args, path, timeout=20):
    """Return (exit status, stdout bytes).  Status outside 0/1/2 is a crash."""
    try:
        r = subprocess.run([bin_] + args + [path], capture_output=True,
                           timeout=timeout)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return 124, b""
    except OSError as e:
        print("cannot run %s: %s" % (bin_, e), file=sys.stderr)
        sys.exit(2)


def mode_score(root, bin_):
    tests = load(root)
    rows = []
    for t in tests:
        oos = classify(t)
        st, _ = run(bin_, [], t["PATH"])
        rows.append((t, oos, st))

    missing = sum(1 for t, o, s in rows if o == "missing")
    print("=== W3C XML Conformance Test Suite, xmlts20130923")
    print("CASES %d catalogued, %d of them not present in the archive"
          % (len(tests), missing))

    # A status outside {0,1,2} is a crash or a hang and is a failure no matter
    # what the case asked for.  A harness written as `! xmlc "$f"` scores a
    # segfault on a not-wf case as a pass; this one does not.
    crash = [t["ID"] for t, o, s in rows if s not in (0, 1, 2, 3)]
    print("CRASHES %d" % len(crash))
    for i in sorted(crash):
        print("CRASH %s" % i)

    print()
    print("--- in scope: XML 1.0 5th ed, non-validating, no external "
          "entities, UTF-8")
    inscope = [(t, s) for t, o, s in rows if o is None]
    bad = []
    for ty in ("not-wf", "valid", "invalid", "error"):
        grp = [(t, s) for t, s in inscope if t["TYPE"] == ty]
        want = WANT[ty]
        if want is None:
            rej = sum(1 for t, s in grp if s == 1)
            dec = sum(1 for t, s in grp if s == 3)
            print("SCORE error    %d cases: %d rejected, %d accepted, "
                  "%d declined (either verdict conforms)"
                  % (len(grp), rej, len(grp) - rej - dec, dec))
            continue
        # A DECLINE (exit 3) is not a verdict, and counting it as one is
        # how this score became meaningless. `not-wf 897/897 rejected` was
        # 799 declines and 98 real rejections: 89% of the headline number
        # was xmlc saying it does not implement comments, processing
        # instructions, CDATA or the doctype yet.
        #
        # PLAN.md's first commit entry names this failure mode -- "a stub
        # that rejects every input scores 1498/1498 on not-wf, which proves
        # that a not-wf score alone measures nothing" -- and the score was
        # 89% of the way to being that stub with nothing in the output
        # saying so. xmlc now exits 3 for a decline so the two can be told
        # apart.
        declined = [t for t, s in grp if s == 3]
        judged = [(t, s) for t, s in grp if s != 3]
        good = [t for t, s in judged if s == want]
        verb = "rejected" if want else "accepted"
        print("SCORE %-8s %4d/%-4d %s, of %d judged (%d declined as "
              "unimplemented)"
              % (ty, len(good), len(judged), verb, len(judged), len(declined)))
        bad += [t for t, s in judged if s != want]
    # The proportion in one line, because that is the number a reader needs
    # and the per-type lines above make them do arithmetic for it.
    ndec = sum(1 for t, s in inscope if s == 3)
    njud = len(inscope) - ndec
    print("DECLINED %d of %d in-scope cases (%.0f%%) are unimplemented "
          "productions, not verdicts" % (ndec, len(inscope),
                                         100.0 * ndec / max(1, len(inscope))))
    print("JUDGED %d" % njud)
    print("SHOULD-HAVE-CAUGHT %d" % len(bad))
    for t in sorted(bad, key=lambda t: t["ID"]):
        print("FAIL %s %s %s" % (t["TYPE"], t["ID"], t["URI"]))

    print()
    print("--- out of scope, by the suite's own classification of its own "
          "tests")
    cnt = collections.Counter(o for t, o, s in rows if o)
    agree = collections.Counter()
    for t, o, s in rows:
        if o and WANT[t["TYPE"]] is not None and s == WANT[t["TYPE"]]:
            agree[o] += 1
    for k in sorted(OOS_REASON):
        # "match the catalogue" is deliberately not called "pass".  For the
        # edition1-4 group matching is not even desirable: those cases are
        # not-wf under editions 1 to 4 and well-formed under the fifth, so a
        # correct fifth-edition parser is supposed to disagree with them.
        print("OOS %-11s %4d cases, %4d match the catalogue anyway  (%s)"
              % (k, cnt[k], agree[k], OOS_REASON[k]))
    print("OUT-OF-SCOPE %d" % sum(cnt.values()))
    return 1 if crash else 0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    mode, root, bin_ = sys.argv[1], sys.argv[2], sys.argv[3]
    if mode == "score":
        return mode_score(root, bin_)
    if mode in ("canon", "diff"):
        item = "28, the canonical dump" if mode == "canon" else "36, the expat differential"
        print("%s mode is not implemented yet (PLAN item %s)" % (mode, item),
              file=sys.stderr)
        return 3
    print("unknown mode %s" % mode, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
