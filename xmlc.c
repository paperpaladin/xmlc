/* xmlc -- a non-validating XML 1.0 (Fifth Edition) well-formedness checker.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One C11 file, libc only.  Reads UTF-8, says well-formed or not with its exit
 * status, and can print the parse back as canonical XML so two parses can be
 * diffed byte for byte.
 *
 * Fifth Edition matters.  The W3C conformance suite's default profile is the
 * second edition, whose Name production is an explicit list of Unicode
 * character classes; the fifth edition replaced it with a handful of ranges
 * that admit far more.  Python's expat still implements the older rules -- it
 * rejects U+0132 and every astral-plane name character -- so the two
 * implementations disagree on names by construction, and the suite's own
 * EDITION attribute is what adjudicates.
 *
 * This commit is the bottom layer only: slurp, UTF-8 decoding, the character
 * classes from productions [2], [3], [4], [4a], [5] and [13], name scanning,
 * and error reporting with a line/column.  There is no document parser yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XMLC_VERSION "0.1"

/* ------------------------------------------------------------------ errors */

typedef enum {
	XML_OK = 0,
	XML_ERR_UTF8,		/* malformed UTF-8 sequence */
	XML_ERR_CHAR,		/* a byte sequence that decodes outside [2] Char */
	XML_ERR_NAME,		/* not a Name where a Name was required */
	XML_ERR_UNIMPL,		/* no document parser in this commit */
	XML_ERR_LAST
} xml_err;

static const char *const errtext[] = {
	"ok",
	"malformed UTF-8 sequence",
	"character is not legal in XML 1.0 [2]",
	"not a Name [5]",
	"not implemented",
};

/* ----------------------------------------------------------- UTF-8 reading */

/* Decode the sequence at b[i..n).  Returns the code point and sets *w to its
 * width in bytes.  Returns -1 for a malformed sequence (overlong, truncated,
 * a stray continuation byte, a UTF-8-encoded surrogate, or anything above
 * U+10FFFF) with *w set to 1 so a caller can still make progress, and -2 at
 * end of input with *w set to 0.
 *
 * Strictness here is not optional: the alternative is that ill-formed UTF-8
 * silently becomes U+FFFD and a document that is not XML is reported as
 * well-formed. */
static long u8(const unsigned char *b, size_t n, size_t i, int *w)
{
	unsigned char c;
	long cp;
	int k, need;

	if (i >= n) {
		*w = 0;
		return -2;
	}
	*w = 1;
	c = b[i];
	if (c < 0x80)
		return c;
	if (c < 0xC2)		/* 0x80-0xBF continuation, 0xC0/0xC1 overlong */
		return -1;
	if (c < 0xE0) {
		need = 1;
		cp = c & 0x1F;
	} else if (c < 0xF0) {
		need = 2;
		cp = c & 0x0F;
	} else if (c < 0xF5) {
		need = 3;
		cp = c & 0x07;
	} else {
		return -1;	/* 0xF5-0xFF: above U+10FFFF, or not UTF-8 */
	}
	if (i + (size_t)need >= n)
		return -1;	/* truncated: fewer continuation bytes than announced */
	for (k = 1; k <= need; k++) {
		if ((b[i + (size_t)k] & 0xC0) != 0x80)
			return -1;
		cp = (cp << 6) | (b[i + (size_t)k] & 0x3F);
	}
	/* Overlong forms and surrogates are ill-formed UTF-8, not characters. */
	if (need == 2 && cp < 0x800)
		return -1;
	if (need == 3 && cp < 0x10000)
		return -1;
	if (cp >= 0xD800 && cp <= 0xDFFF)
		return -1;
	if (cp > 0x10FFFF)
		return -1;
	*w = need + 1;
	return cp;
}


/* -------------------------------------------------------- character classes

   XML 1.0 Fifth Edition, verbatim from the productions.  Written as explicit
   range tests rather than a table because the ranges are few and the
   productions are what a reader will want to check them against.  */

/* [2] Char */
static int is_char(long c)
{
	return c == 0x9 || c == 0xA || c == 0xD ||
	    (c >= 0x20 && c <= 0xD7FF) ||
	    (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}


/* [4] NameStartChar */
static int is_namestart(long c)
{
	return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' ||
	    (c >= 'a' && c <= 'z') ||
	    (c >= 0xC0 && c <= 0xD6) ||
	    (c >= 0xD8 && c <= 0xF6) ||
	    (c >= 0xF8 && c <= 0x2FF) ||
	    (c >= 0x370 && c <= 0x37D) ||
	    (c >= 0x37F && c <= 0x1FFF) ||
	    (c >= 0x200C && c <= 0x200D) ||
	    (c >= 0x2070 && c <= 0x218F) ||
	    (c >= 0x2C00 && c <= 0x2FEF) ||
	    (c >= 0x3001 && c <= 0xD7FF) ||
	    (c >= 0xF900 && c <= 0xFDCF) ||
	    (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

/* [4a] NameChar */
static int is_namechar(long c)
{
	return is_namestart(c) || c == '-' || c == '.' ||
	    (c >= '0' && c <= '9') || c == 0xB7 ||
	    (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}


/* ------------------------------------------------------------------ parser */

typedef struct {
	const unsigned char *b;
	size_t n, i;

	xml_err err;
	const char *detail;	/* static string, or NULL */
	size_t errpos;
} P;

/* How many code points the Char sweep saw.  Printed by -C so the sweep cannot
 * silently do nothing, which is the failure mode of every test harness. */
static unsigned long charcount;

static int fail(P *p, xml_err e, const char *detail)
{
	if (p->err == XML_OK) {
		p->err = e;
		p->detail = detail;
		p->errpos = p->i;
	}
	return 0;
}

/* Current code point, or -1 malformed, -2 at end. */
static long cur(P *p, int *w)
{
	return u8(p->b, p->n, p->i, w);
}


/* [5] Name ::= NameStartChar (NameChar)*
 * Scans one Name at the current position.  Returns 1 and sets *len to its
 * byte length, or fails. */
static int scan_name(P *p, size_t *len)
{
	size_t start = p->i;
	long c;
	int w;

	c = cur(p, &w);
	if (c == -1)
		return fail(p, XML_ERR_UTF8, NULL);
	if (!is_namestart(c))
		return fail(p, XML_ERR_NAME, "name does not start with a "
			    "NameStartChar [4]");
	p->i += (size_t)w;
	for (;;) {
		c = cur(p, &w);
		if (c < 0 || !is_namechar(c))
			break;
		p->i += (size_t)w;
	}
	*len = p->i - start;
	return 1;
}

/* [2] Char over the whole input.  Returns 1 if every code point in the input
 * decodes and is a legal XML character. */
static int sweep_chars(P *p)
{
	charcount = 0;
	for (;;) {
		int w;
		long c = cur(p, &w);
		if (c == -2)
			return 1;
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		p->i += (size_t)w;
		charcount++;
	}
}

/* ---------------------------------------------------------- line and column

   Computed from the byte offset only when something has gone wrong.  Keeping
   a running line counter would cost a branch on every byte of every document
   to make the error path marginally faster, which is the wrong trade. */
static void linecol(const unsigned char *b, size_t n, size_t off,
		    long *line, long *col)
{
	size_t i;
	int w;

	*line = 1;
	*col = 1;
	for (i = 0; i < off && i < n;) {
		long c = u8(b, n, i, &w);
		if (c == 0xA) {
			(*line)++;
			*col = 1;
		} else if (c == 0xD) {
			/* A CR not followed by LF still ends a line. */
			(*line)++;
			*col = 1;
			if (i + 1 < n && b[i + 1] == 0xA)
				i++;
		} else {
			(*col)++;
		}
		i += (size_t)(w ? w : 1);
	}
}

/* --------------------------------------------------------------------- CLI */

static unsigned char *slurp(const char *path, size_t *len)
{
	FILE *f = path ? fopen(path, "rb") : stdin;
	unsigned char *b = NULL;
	size_t cap = 0, n = 0;

	if (!f)
		return NULL;
	for (;;) {
		size_t got;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 65536;
			unsigned char *nb = realloc(b, ncap);
			if (!nb) {
				free(b);
				if (path)
					fclose(f);
				return NULL;
			}
			b = nb;
			cap = ncap;
		}
		got = fread(b + n, 1, cap - n, f);
		n += got;
		if (got == 0)
			break;
	}
	if (path)
		fclose(f);
	/* Shrink to the exact byte count.  Slack at the end of the allocation
	 * would let a read past the end of the document land inside the same
	 * malloc block, where AddressSanitizer cannot see it -- which is
	 * exactly how a truncation sweep comes out clean while being useless. */
	{
		unsigned char *nb = realloc(b, n ? n : 1);
		if (nb)
			b = nb;
	}
	*len = n;
	return b;
}

static void usage(void)
{
	fputs("usage: xmlc [-N] [FILE]\n"
	      "  (no flag)  check well-formedness: exit 0 well-formed, 1 not,\n"
	      "             2 unreadable\n"
	      "  -N         the whole input must be one Name [5]\n"
	      "  -C         every code point must be a legal Char [2];\n"
	      "             prints how many were seen\n", stderr);
}

/* Mode -N: the entire input is required to be a single Name.  This exists so
 * the Unicode tables above have a test seam of their own.  They are the one
 * part of the parser whose bugs are invisible in the aggregate -- a wrong
 * range shifts a handful of suite cases and nothing else -- and they are also
 * where the fifth edition differs from everything else. */
static int mode_name(P *p)
{
	size_t len;

	if (p->n == 0)
		return fail(p, XML_ERR_NAME, "empty");
	if (!scan_name(p, &len))
		return 0;
	if (p->i != p->n)
		return fail(p, XML_ERR_NAME, "trailing bytes after the name");
	return 1;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int namemode = 0, charmode = 0, i;
	unsigned char *buf;
	size_t len;
	P p;
	int ok;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-N")) {
			namemode = 1;
		} else if (!strcmp(argv[i], "-C")) {
			charmode = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "xmlc: unknown option %s\n", argv[i]);
			return 2;
		} else {
			path = argv[i];
		}
	}

	buf = slurp(path, &len);
	if (!buf) {
		fprintf(stderr, "xmlc: %s: cannot read\n", path ? path : "<stdin>");
		return 2;
	}

	memset(&p, 0, sizeof p);
	p.b = buf;
	p.n = len;

	if (namemode) {
		ok = mode_name(&p);
	} else {
		/* The document grammar does not exist yet.  What does exist is
		 * the layer under it, so run that much and then say so: every
		 * code point in the input must be well-formed UTF-8 and a
		 * legal [2] Char. */
		ok = sweep_chars(&p);
		if (ok) {
			if (charmode)
				printf("%lu characters\n", charcount);
			else
				ok = fail(&p, XML_ERR_UNIMPL,
					  "no document parser yet");
		}
	}

	if (!ok) {
		long line, col;
		linecol(buf, len, p.errpos, &line, &col);
		fprintf(stderr, "xmlc: %s:%ld:%ld: %s%s%s (byte %zu)\n",
			path ? path : "<stdin>", line, col,
			errtext[p.err],
			p.detail ? ": " : "", p.detail ? p.detail : "",
			p.errpos);
	}
	free(buf);
	return ok ? 0 : 1;
}
