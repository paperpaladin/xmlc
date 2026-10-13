#!/bin/sh
# t/run.sh -- build and run every oracle, print the conformance score per
# directory and "N passed, M failed".
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX sh.  This script, not the Makefile, compiles the test programs, so
# `make test-asan` is nothing but a CFLAGS override.
set -u

cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -O2 -g}
SCRATCH=./scratch
SUITE=$SCRATCH/xmlconf
mkdir -p "$SCRATCH"
OUT=$SCRATCH/results.txt
: > "$OUT"

say()  { printf '%s\n' "$*" | tee -a "$OUT"; }
ok()   { say "ok - $*"; }
nok()  { say "not ok - $*"; }

printf '=== building (CC=%s)\n' "$CC"
# shellcheck disable=SC2086
if $CC $CFLAGS -o ./xmlc xmlc.c; then :; else nok "compile xmlc"; fi

# --- character classes and names -------------------------------------------
# These are hand-written from the production text, so they are self-consistency
# and not an oracle; the authority on the fifth-edition Name rules is the
# suite's own EDITION="5" cases, scored below.  They are here because a wrong
# range in a Unicode table moves a handful of suite cases and nothing else,
# which is invisible in an aggregate score.
printf '\n--- productions [2] Char, [4] NameStartChar, [4a] NameChar, [5] Name\n'
nm()  { if printf '%b' "$1" | ./xmlc -N 2>/dev/null; then ok "Name $2"; else nok "Name $2"; fi; }
nnm() { if printf '%b' "$1" | ./xmlc -N 2>/dev/null; then nok "not a Name $2"; else ok "not a Name $2"; fi; }
ch()  { if printf '%b' "$1" | ./xmlc -C >/dev/null 2>&1; then ok "Char $2"; else nok "Char $2"; fi; }
nch() { if printf '%b' "$1" | ./xmlc -C >/dev/null 2>&1; then nok "not a Char $2"; else ok "not a Char $2"; fi; }

nm  'a'                  'single letter'
nm  'A'                  'single capital'
nm  '_'                  'underscore alone'
nm  ':'                  'colon alone is a Name in XML, not in Namespaces'
nm  'a:b'                'colon inside'
nm  'a.b-c_d:e0'         'every ASCII NameChar'
nm  'a0123456789'        'digits after the first character'
nm  '\303\200'           'U+00C0, first non-ASCII NameStartChar'
nm  '\304\262'           'U+0132, which fourth-edition BaseChar omits'
nm  '\313\277'           'U+02FF, top of the [#xF8-#x2FF] range'
nm  '\360\220\200\200'     'U+10000, astral NameStartChar (fifth edition)'
nm  'a\314\200'           'U+0300 is a NameChar but not a NameStartChar'
nm  'a\302\267'           'U+00B7 middle dot as a NameChar'
nm  'a\342\200\277'        'U+203F undertie as a NameChar'
nnm ''                   'empty'
nnm '.a'                 'leading dot'
nnm '-a'                 'leading hyphen'
nnm '0a'                 'leading digit'
nnm 'a b'                'embedded space'
nnm '\314\200a'           'U+0300 cannot start a Name'
nnm '\342\200\277a'        'U+203F cannot start a Name'
nnm '\303\227'            'U+00D7 multiplication sign is in neither range'
nnm '\303\267'            'U+00F7 division sign is in neither range'
nnm '\357\277\276'         'U+FFFE is not even a Char'
nnm '\355\240\200'         'a surrogate encoded as UTF-8'
nnm '\300\200'            'overlong NUL'

ch  'a\011b\012c\015d'     'tab, LF and CR are the only legal control chars'
ch  '\355\237\277'         'U+D7FF, just below the surrogate block'
ch  '\356\200\200'         'U+E000, just above it'
ch  '\357\277\275'         'U+FFFD'
ch  '\364\217\277\277'     'U+10FFFF'
nch '\000'                'NUL'
nch '\001'                'U+0001'
nch '\010'                'backspace'
nch '\013'                'vertical tab'
nch '\014'                'form feed'
nch '\033'                'escape'
nch '\357\277\276'         'U+FFFE'
nch '\357\277\277'         'U+FFFF'
nch '\364\220\200\200'     'U+110000 would be above the Unicode range'
nch '\376\377'            'a UTF-16 BOM read as bytes'

# --- hand-written accept/reject table --------------------------------------
# A deliberate overlap with the conformance suite.  The suite is the authority,
# but if the tarball ever fails to extract these still fail when the parser
# breaks, instead of the whole run quietly passing on nothing.
printf '\n--- documents: elements, tags, attributes, nesting\n'
acc() { if printf '%b' "$1" | ./xmlc 2>/dev/null; then ok "accept $2"; else nok "accept $2"; fi; }
rej() { if printf '%b' "$1" | ./xmlc 2>/dev/null; then nok "reject $2"; else ok "reject $2"; fi; }
# `rej` tests truthiness, so exit 3 satisfies it as readily as exit 1: it
# reads "did not accept", not "rejected". That is the same conflation the
# suite scorer was built to undo, still living here in the unit helpers --
# any production this parser has not learned would pass every rej() below.
# `st` pins the exact status, and the cases added since use it.
st() {
	printf '%b' "$2" | ./xmlc >/dev/null 2>&1
	got=$?
	if [ "$got" = "$1" ]; then ok "$3 (exit $1)"
	else nok "$3: exit $got, wanted $1"; fi
}

acc '<a/>'                  'empty element tag'
acc '<a></a>'               'start and end tag'
acc '<a><b/><b/></a>'       'siblings'
acc '<a><b><c/></b></a>'    'nesting'
acc '<a>text</a>'           'character data'

printf '\n--- comments, processing instructions, CDATA sections\n'
# 0 well-formed, 1 not well-formed, 3 not implemented, pinned exactly: the
# difference between 1 and 3 is the whole point of this parser.
st 0 '<a><!-- c --></a>'           'comment inside an element'
st 0 '<a><!----></a>'              'empty comment'
st 0 '<!-- c --><a/>'              'comment before the root'
st 0 '<a/><!-- c -->'              'comment after the root'
st 1 '<a><!-- a -- b --></a>'      "'--' inside a comment [15]"
st 1 '<a><!---></a>'               "a comment may not end '--->' [15]"
st 1 '<a><!-- unterminated</a>'    'unterminated comment'

st 0 '<a><?pi data?></a>'          'processing instruction'
st 0 '<a><?pi?></a>'               'PI with no data'
st 0 '<?pi x?><a/>'                'PI before the root'
st 0 '<a><?xmlfoo d?></a>'         "'xmlfoo' is a legal PI target [17]"
st 1 '<a><?xml d?></a>'            "'xml' is reserved as a PI target [17]"
st 1 '<a><?XmL d?></a>'            'the reservation is case-insensitive [17]'
st 1 '<a><?pi unterminated</a>'    'unterminated PI'

st 0 '<a><![CDATA[x]]></a>'        'CDATA section'
st 0 '<a><![CDATA[]]></a>'         'empty CDATA section'
st 0 '<a><![CDATA[<b>&amp;]]></a>' 'markup inside CDATA is character data'
st 1 '<a><![CDATA[x</a>'           'unterminated CDATA section'
st 1 '<![CDATA[x]]><a/>'           'CDATA outside any element [43]'
st 1 '<a>]]></a>'                  "']]>' outside CDATA is still refused [14]"

# The XML declaration is production [23], not a PI, though it opens '<?'.
# Routing it through the PI parser made it fail the reserved-target rule, so
# every document carrying one -- most of the suite -- was REJECTED rather
# than declined: 183 confident wrong answers about valid documents. It has
# its own section below now that it is implemented; what stays here is the
# distinction itself, which is what went wrong.
st 0 '<?xml version="1.0"?><a/>'   'a declaration at byte 0 is [23], not a PI'
st 1 ' <?xml version="1.0"?><a/>'  'one byte later it is a PI, so target reserved'
st 1 'x<a/>'                       'character data before the root'
st 1 '<a/><b/>'                    'a second root element'
st 1 '<!-- c -->'                  'a comment alone is not a document [1]'

printf '\n--- the XML declaration [23]\n'
st 0 '<?xml version="1.0"?><a/>'                  'a plain declaration'
st 0 "<?xml version='1.0'?><a/>"                  'single quotes [24]'
st 0 '<?xml version="1.0" encoding="UTF-8"?><a/>' 'with an encoding [80]'
st 0 '<?xml version="1.0" encoding="utf-8"?><a/>' 'encoding names are case-insensitive'
st 0 '<?xml version="1.0" standalone="yes"?><a/>' 'with standalone [32]'
st 0 '<?xml version="1.0" encoding="UTF-8" standalone="no"?><a/>' 'all three, in order'
st 0 '<?xml version = "1.0" ?><a/>'               'Eq allows space either side [25]'
st 1 '<?xml encoding="UTF-8"?><a/>'               'version is required [24]'
st 1 '<?xml version="2.0"?><a/>'                  "a version number is '1.' digits [26]"
st 1 '<?xml version=1.0?><a/>'                    'the version must be quoted [24]'
st 1 '<?xml version="1.0" standalone="maybe"?><a/>' "standalone is 'yes' or 'no' [32]"
st 1 '<?xml version="1.0" standalone="yes" encoding="UTF-8"?><a/>' 'the order is fixed [23]'
st 1 '<?xml version="1.0" encoding="1bad"?><a/>'  'an encoding name starts with a letter [81]'
st 1 '<?xml version="1.0"<a/>'                    'unterminated declaration'
# Both of these are W3C case o-p32fail3's shape: [80] and [32] each BEGIN
# with S, so the whitespace is part of the production, not decoration. This
# accepted both until the suite said otherwise.
st 1 '<?xml version="1.0"standalone="yes"?><a/>'  'S is required before standalone [32]'
st 1 '<?xml version="1.0"encoding="UTF-8"?><a/>'  'S is required before encoding [80]'
# Declared but unreadable is not the same as malformed.
st 3 '<?xml version="1.10"?><a/>'                 'XML 1.10 is a legal version this parser will not judge [26]'
st 3 '<?xml version="1.0" encoding="EUC-JP"?><a/>' 'a declared encoding xmlc cannot decode [80]'

printf '\n--- references [66] [67] [68]\n'
st 0 '<a>&amp;</a>'                    'the predefined &amp;'
st 0 '<a>&amp;&lt;&gt;&apos;&quot;</a>' 'all five predefined entities'
st 0 '<a>&#65;</a>'                    'decimal character reference [66]'
st 0 '<a>&#x41;</a>'                   'hexadecimal character reference [66]'
st 0 '<a b="&amp;"/>'                  'an entity reference in an attribute value'
st 0 '<a b="&#65;"/>'                  'a character reference in an attribute value'
st 1 '<a>&foo;</a>'                    'undeclared entity (WFC: Entity Declared) [68]'
st 1 '<a b="&foo;"/>'                  'undeclared entity in an attribute value'
st 1 '<a>&amp</a>'                     "a reference needs its ';' [68]"
st 1 '<a>&#;</a>'                      'a character reference needs a digit [66]'
st 1 '<a>&</a>'                        'a bare & is not a reference'
st 1 '&amp;<a/>'                       'a reference is content, so it needs an element [43]'
# WFC: Legal Character. Each of these parses perfectly and names a code point
# that may not appear in an XML document, which is the whole constraint.
st 1 '<a>&#0;</a>'                     'character reference to NUL [66]'
st 1 '<a>&#xD800;</a>'                 'character reference to a surrogate [66]'
st 1 '<a>&#x110000;</a>'               'character reference past the Unicode range [66]'
st 1 '<a>&#99999999999999;</a>'        'a character reference too large to hold [66]'

printf '\n--- the README documents exit statuses, so run them\n'
# README.md tabulates documents against the status xmlc gives them. That is a
# test suite nobody runs, and it had already gone stale once -- it described
# comments, PIs, CDATA and the declaration as unimplemented after all four
# landed. Every row of the form "<doc>   N   comment" is executed here and
# its status compared, so the file cannot drift from the binary it describes.
readme_rows=0
readme_bad=0
while IFS= read -r line; do
	# A heredoc fed by a command substitution that matched nothing still
	# delivers one empty line, which counted as a row and left the floor
	# below satisfied by nothing at all -- reporting "'' gives 1, the file
	# says" instead of saying the table format had changed.
	[ -n "$line" ] || continue
	doc=${line%%@@*}
	want=${line##*@@}
	printf '%s' "$doc" | ./xmlc >/dev/null 2>&1
	got=$?
	readme_rows=$((readme_rows + 1))
	if [ "$got" != "$want" ]; then
		readme_bad=$((readme_bad + 1))
		nok "README: '$doc' gives $got, the file says $want"
	fi
done <<EOF
$(sed -nE 's/^(<[^ ].*[^ ])[[:space:]]{2,}([013])[[:space:]]{2,}[^ ].*$/\1@@\2/p' README.md)
EOF
if [ "$readme_rows" -lt 15 ]; then
	nok "README: only $readme_rows documented statuses found, expected at least 15 (did the table format change?)"
elif [ "$readme_bad" -eq 0 ]; then
	ok "README: all $readme_rows documented exit statuses match the binary"
fi
acc '<a b="1" c="2"/>'      'two attributes'
acc "<a b='1'/>"            'single-quoted attribute value'
acc '<a b="1\047"/>'        "an apostrophe inside a double-quoted value"
acc "<a b='1\042'/>"        'a quote inside a single-quoted value'
acc '<a b=""/>'             'empty attribute value'
acc '<a  b="1"  />'         'extra whitespace in a start tag'
acc '<a\n/>'                'newline inside a start tag'
acc '<a></a\t\n >'          'whitespace before the end tag bracket'
acc ' \t\r\n<a/>\t\n '      'whitespace around the root'
acc '<a:b c:d="1"/>'        'colons are Name characters in XML itself'
acc '<_/>'                  'underscore element name'
rej ''                      'empty document'
rej '  '                    'whitespace only'
rej '<a>'                   'unclosed root'
rej '</a>'                  'end tag with nothing open'
rej '<a></b>'               'mismatched end tag'
rej '<a><b></a></b>'        'improperly nested'
rej '<a/><b/>'              'two root elements'
rej '<a/>x'                 'character data after the root'
rej 'x<a/>'                 'character data before the root'
rej '<a/'                   'truncated empty element tag'
rej '<a'                    'truncated start tag'
rej '<'                     'a lone left angle bracket'
rej '<a b="1" b="2"/>'      'the same attribute twice (WFC: Unique Att Spec)'
rej '<a b="<"/>'            'a literal < in an attribute value'
rej '<a b=1/>'              'unquoted attribute value'
rej '<a b="1\047/>'         'mismatched attribute value quotes'
rej '<a b/>'                'attribute with no value'
rej '<a b="1"c="2"/>'       'no whitespace between attributes'
rej '<a/ >'                 'space inside the empty element close'
rej '< a/>'                 'space before the element name'
rej '<a>\001</a>'           'a control character in character data'
rej '<a b="\001"/>'         'a control character in an attribute value'
rej '<0a/>'                 'element name starting with a digit'
rej '<a></a >x'             'content after the root, after whitespace'

# --- oracle 2a: the UTF-8 decoder against Python -------------------------
printf '\n--- oracle 2: Python decides what is UTF-8\n'
python3 t/diff.py utf8 ./xmlc 2>&1 | tee -a "$OUT"

# --- the W3C conformance suite ---------------------------------------------
# Vendored whole and unmodified; see t/XMLTS-PROVENANCE for why a subset would
# not have been licensable.  t/suite.py drives it from the catalogue.
printf '\n--- oracle 1: W3C XML Conformance Test Suite\n'
# The corpus is the oracle, so verify it is the corpus we recorded before
# scoring against it. t/XMLTS-PROVENANCE pins the sha256 of the upstream
# tarball; a score against a tampered or truncated archive is worth nothing,
# and until now the recorded digest was never read by anything.
want=$(sed -n 's/^sha256: *//p' t/XMLTS-PROVENANCE)
got=$(sha256sum t/xmlts20130923.tar.gz | cut -d" " -f1)
if [ -z "$want" ]; then
	nok "t/XMLTS-PROVENANCE records no sha256"
elif [ "$want" = "$got" ]; then
	ok "conformance tarball matches the sha256 in t/XMLTS-PROVENANCE"
else
	nok "conformance tarball digest $got != recorded $want"
fi

if [ ! -d "$SUITE" ]; then
	tar xzf t/xmlts20130923.tar.gz -C "$SCRATCH" || nok "extract suite"
fi
if [ -d "$SUITE" ]; then
	python3 t/suite.py score "$SUITE" ./xmlc > "$SCRATCH/score.txt" 2>&1
	cat "$SCRATCH/score.txt" | tee -a "$OUT"
	# Pin the numbers.  A regression now fails the suite instead of
	# quietly scoring lower, which is the only way a score is a test.
	# DECLINED and JUDGED are pinned too. They are the two numbers that say
	# how much of the corpus xmlc actually has an opinion about, and without
	# them the SCORE lines read as a conformance result when 93% of the
	# in-scope cases are declined as unimplemented.
	grep -E '^(CASES|CRASHES|SCORE|DECLINED|JUDGED|SHOULD-HAVE-CAUGHT|OOS|OUT-OF-SCOPE|FAIL) ' \
		"$SCRATCH/score.txt" > "$SCRATCH/score.pinned"
	if [ ! -f t/suite.expected ]; then
		nok "t/suite.expected missing (cp scratch/score.pinned t/suite.expected)"
	elif diff -u t/suite.expected "$SCRATCH/score.pinned" >> "$OUT" 2>&1; then
		ok "W3C suite score matches t/suite.expected"
	else
		nok "W3C suite score differs from t/suite.expected"
		diff -u t/suite.expected "$SCRATCH/score.pinned" | head -40
	fi
else
	nok "suite not extracted"
fi

# --- tally -----------------------------------------------------------------
P=$(grep -c '^ok - ' "$OUT")
F=$(grep -c '^not ok - ' "$OUT")
printf '\n'
printf '%s passed, %s failed\n' "$P" "$F"
[ "$F" -eq 0 ]
