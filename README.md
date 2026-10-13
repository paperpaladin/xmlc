# xmlc

An XML 1.0 (Fifth Edition) well-formedness checker in one C11 file.

**It does not implement the prolog, comments, processing instructions or
CDATA sections.** It declines 93% of the W3C conformance suite rather than
guessing at it, and the exit status says which kind of answer you got:

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
SCORE not-wf     98/98   rejected, of 98 judged (799 declined as unimplemented)
SCORE valid       0/1    accepted, of 1 judged (597 declined as unimplemented)
SCORE invalid    20/21   accepted, of 21 judged (150 declined as unimplemented)
SCORE error    8 cases: 0 rejected, 1 accepted, 7 declined (either verdict conforms)
DECLINED 1553 of 1674 in-scope cases (93%) are unimplemented productions, not verdicts
JUDGED 121
```

The count of cases the parser *should* have caught and did not went from 749
to 2 -- not because anything was fixed, but because the number finally meant
something. `897/897` became `98/98 of 121 judged`, which is a smaller claim
and a true one.

`valid 0/1` is there on purpose too: of the one valid document currently in
scope, `xmlc` accepts none. That is a real gap, printed every run.

## The oracle

The W3C XML Conformance Test Suite -- 1674 in-scope cases, plus 911 excluded
because they target XML 1.1 or namespaces, which this does not claim. The
vendored tarball is verified by sha256 before use, so the corpus cannot drift
underneath the score.

`make test`: **84 passed, 0 failed.** The suite also pins the score, so a
change in what is judged or declined fails rather than silently moving the
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
<a>x</a>             0    well-formed
<a>x</b>             1    mismatched tags
<a/><b/>             1    two roots
<a x="1" x="2"/>     1    duplicate attribute
<a>]]></a>           1    the ']]>' prohibition in character data
```

- the UTF-8 decoder and the Fifth Edition `Char`, `NameStartChar` and
  `NameChar` classes
- element structure: tag matching and the single-root rule
- attributes, including duplicate detection
- character data

## What is declined, exactly

```
<a>&amp;</a>                     3    entity reference
<a>&#65;</a>                     3    character reference
<a><!-- c --></a>                3    comment
<a><![CDATA[x]]></a>             3    CDATA section
<a><?pi x?></a>                  3    processing instruction
<?xml version="1.0"?><a/>        3    XML declaration
<!DOCTYPE a><a/>                 3    DOCTYPE
```

References are declined too -- including the five predefined entities, so even
`&amp;` gets exit 3. Together with the prolog and comments that is why 93% of
the in-scope suite is declined rather than judged, and it is the honest shape
of an incomplete parser: the productions it has not been taught are the ones
it says nothing about.

## Still to come

The prolog (XML declaration, DOCTYPE), comments, processing instructions and
CDATA sections -- which between them account for most of the 1553 declined
cases. `PLAN.md` holds the ordered list; each item names the productions it
would add and the score it would move.

GPL-3.0-or-later. The vendored W3C suite keeps its own licence beside it.
