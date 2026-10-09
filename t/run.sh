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

# --- the W3C conformance suite ---------------------------------------------
# Vendored whole and unmodified; see t/XMLTS-PROVENANCE for why a subset would
# not have been licensable.  t/suite.py drives it from the catalogue.
printf '\n--- oracle 1: W3C XML Conformance Test Suite\n'
if [ ! -d "$SUITE" ]; then
	tar xzf t/xmlts20130923.tar.gz -C "$SCRATCH" || nok "extract suite"
fi
if [ -d "$SUITE" ]; then
	python3 t/suite.py score "$SUITE" ./xmlc > "$SCRATCH/score.txt" 2>&1
	cat "$SCRATCH/score.txt" | tee -a "$OUT"
	# Pin the numbers.  A regression now fails the suite instead of
	# quietly scoring lower, which is the only way a score is a test.
	grep -E '^(CASES|CRASHES|SCORE|SHOULD-HAVE-CAUGHT|OOS|OUT-OF-SCOPE|FAIL) ' \
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
