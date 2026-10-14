# PLAN

## Contract

`xmlc` is one C11 file that checks an XML 1.0 (Fifth Edition) document for
well-formedness and, on request, prints it back in James Clark's canonical XML
form so two parses can be diffed byte for byte.

- `xmlc FILE` says well-formed or not with its exit status and nothing else:
  0 well-formed, 1 not well-formed, 2 cannot read the file, 3 the document
  uses a production that is not implemented yet.  One diagnostic line on
  stderr naming the line, column and byte offset.
  Status 3 exists because it has to: while the parser is incomplete, "I
  reject this" and "I have not been taught this" are different answers, and
  collapsing them into exit 1 makes a stub score 100% on `not-wf`.  It is a
  scaffolding status and goes away when the last production lands.
- `xmlc -c FILE` writes canonical XML (the `canonxml.html` grammar shipped
  with the conformance suite): UTF-8, no prolog, no DTD, no comments,
  `<e></e>` for empty elements, attributes in Unicode code point order,
  `&amp; &lt; &gt; &quot; &#9; &#10; &#13;` and nothing else escaped.
- `xmlc -e FILE` writes one line per parse event (`SE`, `AT`, `TX`, `PI`,
  `CM`, `EE`) so the event sequence can be compared against another
  implementation.
- `xmlc -n FILE` is `-e` plus namespace resolution: expanded names as
  `{uri}local`, undeclared prefixes an error.
- `xmlc -S FILE` parses every truncation of the file, for the sanitizers.
- Internal DTD subset is parsed: `ENTITY`, `ELEMENT`, `ATTLIST`, `NOTATION`,
  parameter entities, and attribute defaults are supplied.  Internal general
  entities are expanded.
- Not in the contract: DTD *validation*, external entities or an external DTD
  subset (not fetched, and their absence is handled per the spec's
  relaxations), XML 1.1, XML Schema, Relax NG, XInclude, XPath, UTF-16, any
  encoding beyond UTF-8 and US-ASCII, XML Canonicalisation (C14N: a
  different thing from the canonical form above), or a mutable document tree.

## Status, as of the last commit

Two of the 42 items below are done: the catalogue-driven scorer and the UTF-8
reader with the Char/Name classes.  Elements parse well enough to produce a
verdict, but comments, processing instructions, CDATA, the prolog, references,
the DTD subset and namespaces do not, so **93% of the in-scope suite is
declined with exit 3, not judged** (1553 of 1674).  Of the 121 cases
xmlc does judge, 119 agree with the suite.

The sections below describe the finished tool.  They are written in the
present tense because they are a specification, not a report. Of the four
oracles, only the first is wired up.  The expat differential, the sanitizer
truncation sweep and the canonical round-trip are items 36, 39 and 28, all
still open, and `t/run.sh` does not run them.  Nothing in this repo has
measured memory safety yet.

## Hard invariant

**Acceptance is exactly the XML 1.0 Fifth Edition well-formedness grammar plus
the Namespaces 1.0 constraints, and no input can make the parser crash, read
out of bounds, leak, or expand without bound.**

Both halves are checked by something outside this repo.  The first half is
scored against the W3C XML Conformance Test Suite: 2585 catalogued cases we
did not write, with the score published per directory and every failure named
and classified as "should have caught" or "out of scope for a non-validating
parser".  The second half is checked by a truncation sweep under
`-fsanitize=address,undefined` and by an entity expansion cap that turns a
billion-laughs bomb into an error return, not an OOM.

Not aiming at 100%. The suite contains DTD-validation cases, XML 1.1
cases, fourth-edition name rules and external-entity cases that a
non-validating UTF-8-only XML 1.0 Fifth Edition parser is not required to
catch. Claiming those would be dishonest, so the scope filter is written in
terms of the suite's *own* `TYPE`, `ENTITIES`, `VERSION`, `EDITION` and
`RECOMMENDATION` attributes and the two counts are printed side by side.

## Oracle

Ranked by how little we control them.

1. The W3C XML Conformance Test Suite (`xmlts20130923`, vendored whole and
   unmodified; see `t/XMLTS-PROVENANCE`).  2585 cases from Sun, OASIS/NIST,
   IBM, Fuji Xerox, James Clark and the University of Edinburgh, with a
   machine-readable catalogue that classifies each one.  `valid/` must be
   accepted, `not-wf/` must be rejected, `invalid/` must be *accepted* because
   a non-validating parser is not asked to notice, `error/` may go either way.
   120 of the `valid/sa` cases ship an expected canonical form, a
   second, stronger check than accept/reject: it compares the parse.
2. Python's `xml.parsers.expat`.  A second independent implementation in
   the standard library, written by James Clark, differentialled two ways:
   accept/reject over the whole suite, and the full parse event sequence over
   every mutually accepted document plus generated ones.  Disagreements have
   to be adjudicated against the spec and pinned, not papered over.
3. The sanitizers.  `-fsanitize=address,undefined` over a byte-by-byte
   truncation sweep and over the whole suite.  Not a correctness oracle, but
   the only authority on memory safety that is not our own opinion.
4. Canonical round-trip.  parse -> canonical form -> parse -> canonical
   form must be a fixed point, and the second must be byte-identical.
   Self-consistency only, so it ranks last.

## Non-goals

- DTD validation.  No content model checking, no `ID`/`IDREF` resolution,
  no required-attribute enforcement.  `invalid/` cases are accepted on
  purpose. That is what "non-validating" means.
- External entities and the external DTD subset.  Never fetched, not even
  from the local filesystem.  Where the spec relaxes a constraint because the
  processor did not read the external subset, `xmlc` takes the relaxation.
- XML 1.1.  `version="1.1"` is rejected.  NEL and the extra name
  characters are not implemented.
- Encodings other than UTF-8 and US-ASCII.  UTF-16 is detected from its
  BOM and rejected with a named error, not misread.
- Schema languages, XInclude, XPath, XSLT, C14N, XML Catalogs.
- Streaming.  One buffer in, one verdict out.

## Commit sequence

- [x] vendor the suite, build the catalogue-driven scorer, and pin a stub
  - why: the scorer is the deliverable and it has to be watched failing
    first.  A stub that rejects every input scores 1498/1498 on `not-wf`,
    which proves that a `not-wf` score alone measures nothing.  Both numbers
    get pinned from the start so the climb is in the history.
- [x] UTF-8 reader, Char/NameStartChar/NameChar classes, name scanning
  - why: every later production is written in terms of these, and Fifth
    Edition name rules are the single biggest behavioural difference from the
    suite's default (2nd edition) profile.
- [x] elements, start and end tags, attributes, proper nesting
  - why: the smallest thing that accepts a document, so `valid` leaves zero.
- [x] character data and the forbidden `]]>`
  - why: `]]>` outside CDATA is the one content error with no local cause.
- [x] comments and processing instructions
  - why: `--` inside a comment and `<?xml` as a PI target are both errors.
- [x] CDATA sections
  - why: the only place `]]>` terminates, not offends.
- [x] the XML declaration (the doctype and the rest of the prolog remain)
  - why: version, encoding and standalone each have their own grammar, and
    `encoding="UTF-16"` is where "reject what you cannot do" lands.
- [x] character and entity references (attribute value normalisation is not observable without a value API, so it is not done)
  - why: `&#x0;`, surrogates and `&#xFFFE;` are invalid characters, not
    invalid syntax, and normalisation is where tab becomes space.
- [ ] internal DTD subset, entity declarations, entity expansion
  - why: the expansion machinery is where the frame stack and the
    same-entity nesting constraint come from.
- [ ] the entity expansion cap
  - why: billion laughs.  A depth and a byte cap, with a test that the bomb
    exits 1 instead of being killed by the OOM reaper.
- [ ] ATTLIST, attribute defaults, non-CDATA attribute normalisation
  - why: a non-validating processor still has to supply defaults from the
    internal subset, and the suite's canonical output files prove it.
- [ ] namespaces
  - why: undeclared prefix, the empty default namespace, reserved `xml:` and
    `xmlns:`, and colons in names that are legal in XML and not in NS.
- [ ] the canonical dump
  - why: turns 120 of the `valid` cases from accept/reject into a byte
    comparison against output we did not write.
- [ ] score the suite, name every failure, split should-have-caught from
      out-of-scope
  - why: this is the headline number and the point of the repo.
- [ ] close the should-have-caught failures
  - why: each one is a real bug; the out-of-scope ones stay, named.
- [ ] the expat differential: accept/reject and event sequence
  - why: a second implementation disagreeing is the only way to find the
    places where we and the suite happen to share a misreading.
- [ ] the truncation sweep and the sanitizers
  - why: a prefix of a valid document exercises every "is there another
    byte?" check at the exact offset where the answer is no.
- [ ] README, the honest list of what does not work
  - why: a README that only lists successes is not finished.
