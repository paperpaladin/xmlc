# xmlc -- SPDX-License-Identifier: GPL-3.0-or-later
#
# t/run.sh compiles the test programs itself, so the sanitizer target is just a
# CFLAGS override.  -ffile-prefix-map keeps the build reproducible: without it
# -g writes $(CURDIR) into DWARF .debug_line_str.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -ffile-prefix-map=$(CURDIR)=.
ASAN     = -std=c11 -Wall -Wextra -Wpedantic -O1 -g -ffile-prefix-map=$(CURDIR)=. \
	   -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan test-valgrind clean

all: xmlc

xmlc: xmlc.c
	$(CC) $(CFLAGS) -o $@ xmlc.c

test: all
	CC='$(CC)' CFLAGS='$(CFLAGS)' ./t/run.sh

# We parse untrusted bytes, expand entities into heap buffers and sweep every
# truncation of a valid document, which is exactly the shape of bug the
# sanitizers catch.  Not optional.
test-asan:
	CC='$(CC)' CFLAGS='$(ASAN)' UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		ASAN_OPTIONS=detect_leaks=1 ./t/run.sh

# `make test-asan` leaves instrumented binaries behind and valgrind refuses to
# run those, so force a clean build first.
test-valgrind:
	rm -f xmlc
	$(MAKE) xmlc
	CC='$(CC)' CFLAGS='$(CFLAGS)' XMLC_VALGRIND=1 ./t/run.sh

clean:
	rm -f xmlc t/unit
	rm -rf scratch
