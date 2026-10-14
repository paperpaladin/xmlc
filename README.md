# xmlc

An XML 1.0 (Fifth Edition) well-formedness checker in one C11 file.

It does not implement the DOCTYPE declaration or its internal subset. It
declines 82% of the W3C conformance suite instead of guessing at it, and the
exit status says which kind of answer you got:

```
xmlc FILE
  0  well-formed
  1  not well-formed          (one diagnostic line: line, column, byte offset)
  2  cannot read the file
  3  uses a production that is not implemented yet
```

Status 3 exists because it has to. While the parser is incomplete, "I reject
this" and "I have not been taught this" are different answers, and a checker
that conflates them is not reporting anything.

## Why that distinction is the whole repository

This suite used to print `897/897`.

That number was meaningless. The harness scored a "not implemented" exit the
same as "well-formed", so every W3C case using a production the parser could
not read counted as a pass. Separating the two gave:

```
SCORE not-wf    227/227  rejected, of 227 judged (649 declined as unimplemented)
SCORE valid       0/1    accepted, of 1 judged (590 declined as unimplemented)
SCORE invalid    58/58   accepted, of 58 judged (96 declined as unimplemented)
SCORE error    6 cases: 0 rejected, 1 accepted, 5 declined (either verdict conforms)
DECLINED 1340 of 1627 in-scope cases (82%) are unimplemented productions, not verdicts
JUDGED 287
SHOULD-HAVE-CAUGHT 1
```

The count of cases the parser *should* have caught and did not went from 749
to 2: not because anything was fixed, but because the number finally meant
something. `897/897` became `98/98 of 121 judged`, a smaller claim
and a true one. Comments, processing instructions, CDATA sections, the XML
declaration and references have since taken it to 287 judged with the same
should-have-caught count of 1, judging well over twice as many cases
without getting any more of them wrong.

`valid 0/1` is there on purpose too: of the one valid document currently in
scope, `xmlc` accepts none. That is a real gap, printed every run.

Adding the XML declaration demonstrated the point twice over. Routed through
the PI parser. It does open `<?`, it failed production [17]'s reserved
target rule, so every document carrying a declaration was *rejected* as
malformed: 676 judged, 183 of them confidently wrong. It is production [23],
not a PI, and `xml` is reserved as a target precisely so the two cannot be
confused. Then the suite caught `<?xml version="1.0"standalone="yes"?>` being
accepted, because [32] *begins* with whitespace and the check did not require
any.

## The oracle

The W3C XML Conformance Test Suite: 1674 in-scope cases, plus 911 excluded
because they target XML 1.1 or namespaces, which this does not claim. The
vendored tarball is verified by sha256 before use, so the corpus cannot drift
underneath the score.

Namespace tests are excluded. Namespaces in XML is a separate
recommendation: a document can be flawless XML 1.0 and break every rule in it,
so judging them would answer a question this parser was not asked. The
catalogue marks them `RECOMMENDATION=NS1.0` itself, out of scope has to be
a claim the suite makes about its own test, not an excuse invented for a
failure, and the same reasoning already excluded NS1.1.

`make test`: **143 passed, 0 failed.** The suite also pins the score, so a
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
  constraints: a character reference must denote a `Char`, and an entity must
  have been declared, which with no DTD means the five predefined ones

## What is declined, exactly

```
<!DOCTYPE a><a/>                  3    DOCTYPE and the internal subset
<?xml version="1.10"?><a/>        3    a legal version this parser will not judge
<?xml version="1.0" encoding="EUC-JP"?><a/>   3   an encoding it cannot decode
```

The DOCTYPE is most of the remaining 82%, and it is the next thing: almost
every `valid` case in the suite carries one.

The last two matter for a different reason: those documents are not malformed,
they are *unreadable by this parser*. Rejecting them would be a claim about
the document. Declining them is a claim about the parser, and only one of
those is true.

## Still to come

The DOCTYPE declaration and its internal subset, which account for most of
the 1340 declined cases. `PLAN.md` holds the ordered list; each item names the productions it
would add and the score it would move.

GPL-3.0-or-later. The vendored W3C suite keeps its own licence beside it.
