# xmlc

An XML 1.0 (Fifth Edition) well-formedness checker in one C11 file.

It now parses the DOCTYPE declaration and its internal subset, so it declines
**0% of the W3C conformance suite** and judges 1623 of the 1624 in-scope
cases. It used to decline 82% of them. The exit status still says which kind
of answer you got:

```
xmlc FILE
  0  well-formed
  1  not well-formed          (one diagnostic line: line, column, byte offset)
  2  cannot read the file
  3  uses a production that is not implemented yet
```

Status 3 exists because it has to. While the parser is incomplete, "I reject
this" and "I have not been taught this" are different answers, and a checker
that conflates them is not reporting anything. Exactly one in-scope case still
gets it, and it is named below.

## Why that distinction is the whole repository

This suite used to print `897/897`.

That number was meaningless. The harness scored a "not implemented" exit the
same as "well-formed", so every W3C case using a production the parser could
not read counted as a pass. Separating the two gave `98/98 of 121 judged`: a
smaller claim and a true one.

The climb since then has been in the denominator, not the numerator:

```
            judged   declined   should-have-caught
elements       121       1553                    1
+ comments, PIs, CDATA, decl, refs
               287       1340                    1
+ DOCTYPE, entities, expansion
              1623          1                    1
```

The current score:

```
SCORE not-wf    873/873  rejected, of 873 judged (0 declined as unimplemented)
SCORE valid     589/590  accepted, of 590 judged (1 declined as unimplemented)
SCORE invalid   154/154  accepted, of 154 judged (0 declined as unimplemented)
SCORE error    6 cases: 1 rejected, 5 accepted, 0 declined (either verdict conforms)
DECLINED 1 of 1624 in-scope cases (0%) are unimplemented productions, not verdicts
JUDGED 1623
SHOULD-HAVE-CAUGHT 1
FAIL valid rmt-e2e-22 E22.xml
```

Adding the XML declaration demonstrated the point twice over. Routed through
the PI parser. It does open `<?`, it failed production [17]'s reserved
target rule, so every document carrying a declaration was *rejected* as
malformed: 676 judged, 183 of them confidently wrong. It is production [23],
not a PI, and `xml` is reserved as a target precisely so the two cannot be
confused. Then the suite caught `<?xml version="1.0"standalone="yes"?>` being
accepted, because [32] *begins* with whitespace and the check did not require
any.

The DOCTYPE work produced its own version of that. Entity replacement text was
stored as the literal was written, with character references left for the
inclusion to re-scan, on the reasoning that it kept a `<` out of the markup.
It does, and that is the bug. Section 4.5 says the replacement text of
`<!ENTITY e "&#60;foo>">` really is `<foo>`, and including it really does open
a tag that is never closed. Six suite cases say so from four directions
(`not-wf-sa-103`, `-090`, `-092`, and errata-4e 140 and 141), and section 4.6
declaring `lt` as `&#38;#60;` rather than `&#60;` says the same thing: the
double escape is what survives the expansion. The model was wrong and only the
oracle knew.

## The oracle

The W3C XML Conformance Test Suite: 1624 in-scope cases, plus 961 excluded
because they target XML 1.1, namespaces, pre-fifth-edition name rules,
external entities or an encoding this parser does not read. That vendored
tarball is verified by sha256 before use, so the corpus cannot drift
underneath the score.

Namespace tests are excluded. Namespaces in XML is a separate
recommendation. A document can be flawless XML 1.0 and break every rule in it,
so judging them would answer a question this parser was not asked. The
catalogue marks them `RECOMMENDATION=NS1.0` itself, out of scope has to be
a claim the suite makes about its own test, not an excuse invented for a
failure, and the same reasoning already excluded NS1.1.

That filter had a hole. It tested `RECOMMENDATION == "NS1.0"` exactly, and the
catalogue also uses `NS1.0-errata1e` for three cases. Those three were being
scored, and an XML 1.0 parser was marked wrong three times for not
implementing `xmlns`. They were invisible while the DOCTYPE was declined,
because all three carry one. The filter matches the `NS` prefix now.

`make test`: **219 passed, 0 failed.** The suite also pins the score, so a
change in what is judged or declined fails instead of silently moving the
denominator.

## Two narrower modes

```
xmlc -N [FILE]    the whole input must be one Name [5]
xmlc -C [FILE]    every code point must be a legal Char [2]; prints how many
```

These exist because the character classes are the part the W3C suite exercises
hardest, and checking them directly is cheaper than inferring them from a
document verdict.

Canonical XML output is in `PLAN.md` and is *not* implemented. The suite's
`canon` and `diff` modes exit 3 naming the item that would add them, rather
than printing something approximate that could be diffed by mistake.

## What is implemented, exactly

```
<a>x</a>                          0    well-formed
<a><!-- c --></a>                 0    comment [15]
<a><?pi data?></a>                0    processing instruction [16]
<a><![CDATA[<b>&amp;]]></a>       0    CDATA section [18], markup inside is text
<?xml version="1.0"?><a/>         0    XML declaration [23]
<a>&amp;&#65;&#x41;</a>           0    references [66] [68]
<!DOCTYPE a [<!ELEMENT a EMPTY>]><a/>         0    internal subset [28] [45]
<!DOCTYPE a [<!ENTITY e "x">]><a>&e;</a>      0    entity expansion [71] [68]
<!DOCTYPE a [<!ENTITY e "<b/>">]><a>&e;</a>   0    an entity may carry markup
<!DOCTYPE a [<!ENTITY % p "<!ELEMENT a EMPTY>">%p;]><a/>   0   parameter entity [69]
<!DOCTYPE a SYSTEM "a.dtd"><a>&e;</a>         0    unread external subset relaxes WFC Entity Declared

<a>x</b>                          1    mismatched tags
<a/><b/>                          1    a second root element
<a x="1" x="2"/>                  1    duplicate attribute
<a>]]></a>                        1    ']]>' outside CDATA [14]
<a><!-- a -- b --></a>            1    '--' inside a comment [15]
<a><!---></a>                     1    a comment may not end '--->' [15]
<a><?xml d?></a>                  1    'xml' is a reserved PI target [17]
<?xml version="1.0"standalone="yes"?><a/>   1    [32] begins with whitespace
<a>&foo;</a>                      1    undeclared entity (WFC: Entity Declared)
<a>&#0;</a>                       1    not a Char (WFC: Legal Character)
<a>&#xD800;</a>                   1    a surrogate is not a Char either
<!DOCTYPE a [<!ELEMENT a (b|c,d)>]><a/>       1    a group is all '|' or all ',' [49] [50]
<!DOCTYPE a [<!ATTLIST a x BOGUS #IMPLIED>]><a/>   1   not an AttType [54]
<!DOCTYPE a [<![INCLUDE[<!ELEMENT a EMPTY>]]>]><a/>   1   conditional sections are external-subset only [28b]
<!DOCTYPE a [<!ENTITY e "&e;">]><a>&e;</a>    1    WFC: No Recursion [68]
<!DOCTYPE a [<!ENTITY e "<b>">]><a>&e;</a>    1    an entity must contain whole elements (4.3.2)
<!DOCTYPE a [<!ENTITY e "&#60;foo>">]><a>&e;</a>   1   4.5: that replacement text is '<foo>'
<!DOCTYPE a [<!ENTITY % p "x"><!ENTITY e "%p;">]><a/>   1   WFC: PEs in Internal Subset [28a]
<a/><!DOCTYPE a>                  1    the doctype belongs in the prolog [22]
```

- the UTF-8 decoder and the Fifth Edition `Char`, `NameStartChar` and
  `NameChar` classes
- element structure: tag matching and the single-root rule
- attributes, including duplicate detection
- character data, and the `]]>` prohibition that is the other half of [14]
- comments, processing instructions and CDATA sections
- the XML declaration: version, encoding and standalone, each with its own
  grammar and in that fixed order
- character and entity references, with both of their well-formedness
  constraints
- the doctype declaration [28], the internal subset [28b], and all four
  markup declarations parsed against their own grammar: `ELEMENT` with the
  full content-model syntax [45]-[51], `ATTLIST` with every attribute type
  and default form [52]-[60], `ENTITY` general and parameter, internal and
  external, parsed and unparsed [70]-[76], and `NOTATION` [82]-[83]
- general entity expansion in content and in attribute values, parameter
  entity expansion between markup declarations and inside entity values,
  and the six constraints that go with them: Entity Declared (with 4.1's
  relaxations), Parsed Entity, No External Entity References, No Recursion,
  PEs in Internal Subset, and 4.3.2's requirement that an entity contain
  whole elements
- a depth cap and a byte cap on expansion, so a billion-laughs document is
  an error return in 80 milliseconds and 1.3MB rather than an OOM

## What does NOT work

**Attribute defaults are parsed and discarded.** `<!ATTLIST a x CDATA "1">` is
checked for syntax and for every well-formedness constraint its default value
can break, and then thrown away: a document that omits `x` does not get it.
Nothing in the suite's accept/reject score can see that, which is exactly why
it is called out here. The canonical-output cases would see it, and they are
not wired up yet either. Same for non-CDATA attribute-value normalisation.

**No namespaces.** 51 suite cases are excluded for it, by the catalogue's own
`RECOMMENDATION` attribute. `<a:b/>` with no binding for `a:` is accepted,
because it is well-formed XML 1.0.

**No validation.** No content-model checking, no `ID`/`IDREF`, no required
attributes. The content model in `<!ELEMENT>` is parsed and then discarded.
`invalid/` cases are accepted on purpose; that is what non-validating means.

**Nothing external is ever fetched**, not even from the local filesystem. An
external subset or an external entity is noted and skipped. Where the spec
relaxes a constraint because the processor did not read the external subset,
`xmlc` takes the relaxation, which is why
`<!DOCTYPE a SYSTEM "a.dtd"><a>&e;</a>` is accepted and the same document with
`standalone="yes"` is not.

**A UTF-8 BOM is not skipped**, and it is the one remaining
should-have-caught failure: `rmt-e2e-22` is a valid document that xmlc
rejects because the three BOM bytes sit where `<?xml` should. It predates this
work and closing it is its own plan item.

**An ASCII-declared document containing bytes above 0x7F is read as UTF-8**
rather than refused. US-ASCII is a subset of UTF-8 and shares the decoder;
nothing checks that the declaration was telling the truth.

**No canonical output, no expat differential, no sanitizer sweep yet.** Three
of the four oracles in `PLAN.md` are still unwired. `make test-asan` runs the
whole suite under `-fsanitize=address,undefined` and is clean, but the
byte-by-byte truncation sweep that would make that mean something is item 39.

## What is still declined, exactly

```
<?xml version="1.10"?><a/>        3    a legal version this parser will not judge
<?xml version="1.0" encoding="EUC-JP"?><a/>   3   an encoding it cannot decode
```

Those documents are not malformed, they are *unreadable by this parser*.
Rejecting them would be a claim about the document. Declining them is a claim
about the parser, and only one of those is true. One in-scope suite case is in
this state: `x-rmt-008b`, which declares `version="1.7"`.

## Entity expansion caps

Three separate limits, because no one of them covers the others:

- **64 frames** of nesting. The classic billion laughs is only nine deep, so
  this never fires on it; a 200-long chain of one-byte entities is stopped by
  nothing else.
- **8MB of replacement text** included, counted across the whole document.
  This is what stops billion laughs.
- **WFC: No Recursion**, which is a well-formedness constraint and not a cap:
  an entity that reaches itself is a wrong document, not a large one.

All three are tested, and the bomb tests run with the address space capped at
256MB and a 20 second clock, because a bomb test that passes on a machine with
enough memory has tested nothing. Measured with the byte cap compiled out, the
general-entity bomb does not finish inside 20 seconds and the parameter-entity
one allocates 110MB at seven levels and a gigabyte at eight.

## Mutation results

Twenty-two defects were planted in this work, one per rule added, and the
suite had to go red for each. Eighteen did immediately. Four survived, and
all four for the same reason: **a rule whose only job is to give the right
diagnostic cannot be seen by a test that checks the exit status.**

- *WFC: No Recursion* deleted, and a self-referencing entity still exits 1,
  because the depth cap catches the cycle on the 64th frame.
- The conditional-section refusal deleted, and `<![INCLUDE[` still exits 1 as
  "expected a markup declaration".
- The byte cap in the parameter-entity path deleted, and the running total it
  feeds was still enough to trip the cap on the other path. That one is the
  classic vacuous mutation: the line was removed, the counter it wrote to was
  not.
- "The doctype belongs in the prolog" deleted, and `<a/><!DOCTYPE a>` still
  exits 1, because after the root closes a different loop refuses it. The
  check is only reachable from *inside* an element, and there was no test for
  `<a><!DOCTYPE b></a>`.

The first two are now pinned by message rather than by status, the third was
re-planted with the counter removed as well, and the fourth got the test it
was missing. All twenty-two go red now.

GPL-3.0-or-later. The vendored W3C suite keeps its own licence beside it.
