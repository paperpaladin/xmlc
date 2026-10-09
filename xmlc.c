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
 * The parser is iterative, not recursive.  Element nesting is held on an
 * explicit heap stack, so a document 100000 elements deep is an error return
 * rather than a stack overflow, and there is no depth at which the C stack
 * becomes the limit.
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
	XML_ERR_SYNTAX,		/* the grammar says something else goes here */
	XML_ERR_WFC,		/* grammatical, but breaks a well-formedness
				 * constraint */
	XML_ERR_DEPTH,		/* element nesting past XMLC_MAX_DEPTH */
	XML_ERR_MEM,		/* out of memory */
	XML_ERR_UNIMPL,		/* a production this commit does not have yet */
	XML_ERR_LAST
} xml_err;

static const char *const errtext[] = {
	"ok",
	"malformed UTF-8 sequence",
	"character is not legal in XML 1.0 [2]",
	"not a Name [5]",
	"syntax error",
	"well-formedness constraint",
	"too deeply nested",
	"out of memory",
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


/* [3] S */
static int is_s(long c)
{
	return c == 0x20 || c == 0x9 || c == 0xD || c == 0xA;
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

/* A run of bytes inside the document.  Names and attribute values are slices
 * rather than copies wherever they can be: the document is in memory anyway. */
typedef struct {
	const unsigned char *b;
	size_t n;
} slice;

static int sl_eq(slice a, slice b)
{
	return a.n == b.n && (a.n == 0 || memcmp(a.b, b.b, a.n) == 0);
}

/* Growable slice array, used for the element stack and for the attribute list
 * of the start tag being parsed. */
typedef struct {
	slice *v;
	size_t n, cap;
} slicevec;

static int sv_push(slicevec *s, slice x)
{
	if (s->n == s->cap) {
		size_t c = s->cap ? s->cap * 2 : 16;
		slice *v = realloc(s->v, c * sizeof *v);
		if (!v)
			return 0;
		s->v = v;
		s->cap = c;
	}
	s->v[s->n++] = x;
	return 1;
}

/* Element nesting ceiling.  Nothing structural needs one -- the stack is on
 * the heap and the parser is iterative -- but an unbounded one turns a
 * hostile 100MB document into 100MB of stack entries, so there is a number.
 * shortcut: fixed at 1<<20; make it an option if a real document ever wants
 * deeper nesting than a million elements. */
#define XMLC_MAX_DEPTH (1u << 20)

typedef struct {
	const unsigned char *b;
	size_t n, i;

	xml_err err;
	const char *detail;	/* static string, or NULL */
	size_t errpos;

	slicevec stack;		/* open element names, outermost first */
	slicevec att;		/* attribute names of the current start tag */
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

/* The next byte, without decoding.  Markup delimiters are all ASCII, so the
 * dispatch does not need a code point. */
static int byte_at(P *p, size_t k)
{
	return p->i + k < p->n ? p->b[p->i + k] : -1;
}

static int lit(P *p, const char *s)
{
	size_t l = strlen(s);
	return p->i + l <= p->n && memcmp(p->b + p->i, s, l) == 0;
}

static int eat(P *p, const char *s)
{
	size_t l = strlen(s);
	if (!lit(p, s))
		return 0;
	p->i += l;
	return 1;
}


/* [5] Name ::= NameStartChar (NameChar)* */
static int scan_name(P *p, slice *out)
{
	size_t start = p->i;
	long c;
	int w;

	c = cur(p, &w);
	if (c == -1)
		return fail(p, XML_ERR_UTF8, NULL);
	if (!is_namestart(c))
		return fail(p, XML_ERR_NAME, "does not start with a "
			    "NameStartChar [4]");
	p->i += (size_t)w;
	for (;;) {
		c = cur(p, &w);
		if (c < 0 || !is_namechar(c))
			break;
		p->i += (size_t)w;
	}
	out->b = p->b + start;
	out->n = p->i - start;
	return 1;
}

/* [3] S+ ; returns how many characters were skipped. */
static int skip_s(P *p)
{
	int n = 0;
	for (;;) {
		int w;
		long c = cur(p, &w);
		if (c < 0 || !is_s(c))
			return n;
		p->i += (size_t)w;
		n++;
	}
}

/* [25] Eq ::= S? '=' S? */
static int eat_eq(P *p)
{
	skip_s(p);
	if (!eat(p, "="))
		return fail(p, XML_ERR_SYNTAX, "expected '=' after an "
			    "attribute name [25]");
	skip_s(p);
	return 1;
}

/* [10] AttValue ::= '"' ([^<&"] | Reference)* '"'
 *                 | "\'" ([^<&\'] | Reference)* "\'"
 *
 * References are not implemented in this commit; '&' is reported rather than
 * guessed at.  What is already enforced is the constraint that survives every
 * later version of this function: a literal '<' in an attribute value is
 * never well-formed.  That is WFC: No < in Attribute Values, and it is the
 * one rule here with no local syntactic cause -- nothing about '<' is
 * ambiguous, it is simply forbidden. */
static int scan_attvalue(P *p, slice *out)
{
	int q = byte_at(p, 0);
	size_t start;

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, "attribute value must be "
			    "quoted [10]");
	p->i++;
	start = p->i;
	for (;;) {
		int w;
		long c = cur(p, &w);

		if (c == -2)
			return fail(p, XML_ERR_SYNTAX, "unterminated attribute "
				    "value [10]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		if (c == q)
			break;
		if (c == '<')
			return fail(p, XML_ERR_WFC, "'<' in an attribute value "
				    "(WFC: No < in Attribute Values)");
		if (c == '&')
			return fail(p, XML_ERR_UNIMPL, "references in attribute "
				    "values");
		p->i += (size_t)w;
	}
	out->b = p->b + start;
	out->n = p->i - start;
	p->i++;			/* the closing quote */
	return 1;
}

/* [40] STag  ::= '<' Name (S Attribute)* S? '>'
 * [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 * [41] Attribute ::= Name Eq AttValue
 *
 * Returns 1 on success with *empty set.  On entry p->i is at the Name. */
static int parse_stag(P *p, int *empty)
{
	slice name, aname, aval;
	size_t k;

	if (!scan_name(p, &name))
		return 0;
	p->att.n = 0;
	for (;;) {
		int sp = skip_s(p);

		if (eat(p, "/>")) {
			*empty = 1;
			break;
		}
		if (eat(p, ">")) {
			*empty = 0;
			break;
		}
		/* [40] requires S between the name and each attribute, and
		 * between attributes.  <a b="1"c="2"/> is not well-formed. */
		if (!sp)
			return fail(p, XML_ERR_SYNTAX, "expected whitespace, "
				    "'>' or '/>' in a start tag [40]");
		if (!scan_name(p, &aname))
			return 0;
		if (!eat_eq(p))
			return 0;
		if (!scan_attvalue(p, &aval))
			return 0;
		/* WFC: Unique Att Spec.
		 * shortcut: linear scan, so an element with n attributes
		 * costs O(n^2).  A hash would pay off somewhere past a few
		 * hundred attributes on one element, which no document in the
		 * conformance suite comes close to. */
		for (k = 0; k < p->att.n; k++)
			if (sl_eq(p->att.v[k], aname))
				return fail(p, XML_ERR_WFC, "attribute appears "
					    "twice on one element (WFC: Unique "
					    "Att Spec)");
		if (!sv_push(&p->att, aname))
			return fail(p, XML_ERR_MEM, NULL);
	}
	if (!*empty) {
		if (p->stack.n >= XMLC_MAX_DEPTH)
			return fail(p, XML_ERR_DEPTH, NULL);
		if (!sv_push(&p->stack, name))
			return fail(p, XML_ERR_MEM, NULL);
	}
	return 1;
}

/* [42] ETag ::= '</' Name S? '>'  ; on entry p->i is at the Name. */
static int parse_etag(P *p)
{
	slice name;

	if (!scan_name(p, &name))
		return 0;
	skip_s(p);
	if (!eat(p, ">"))
		return fail(p, XML_ERR_SYNTAX, "expected '>' at the end of an "
			    "end tag [42]");
	if (p->stack.n == 0)
		return fail(p, XML_ERR_SYNTAX, "end tag with no open element");
	/* WFC: Element Type Match. */
	if (!sl_eq(p->stack.v[p->stack.n - 1], name))
		return fail(p, XML_ERR_WFC, "end tag does not match the open "
			    "start tag (WFC: Element Type Match)");
	p->stack.n--;
	return 1;
}

/* [14] CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
 * Runs up to the next '<' or '&'.  The ']]>' half of the production arrives
 * with CDATA sections, which are the only place that string is allowed, so
 * the subtraction is enforced here rather than left to the caller. */
static int parse_chardata(P *p)
{
	for (;;) {
		int w;
		long c = cur(p, &w);

		if (c == -2 || c == '<' || c == '&')
			return 1;
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		if (c == ']' && lit(p, "]]>"))
			return fail(p, XML_ERR_SYNTAX, "']]>' must be escaped "
			    "in character data; only a CDATA section may end "
			    "with it");
		p->i += (size_t)w;
	}
}

/* [1]  document ::= prolog element Misc*
 *
 * Iterative.  The element stack is on the heap, so nesting depth never
 * becomes C stack depth; a pathologically deep document is XML_ERR_DEPTH and
 * not a segfault. */
static int parse_document(P *p)
{
	int empty;

	skip_s(p);		/* prolog: XMLDecl, doctype and Misc come later */
	if (byte_at(p, 0) != '<')
		return fail(p, XML_ERR_SYNTAX, "a document must have a root "
			    "element [1]");
	for (;;) {
		if (byte_at(p, 0) == '<') {
			if (byte_at(p, 1) == '/') {
				p->i += 2;
				if (!parse_etag(p))
					return 0;
			} else if (byte_at(p, 1) == '!' || byte_at(p, 1) == '?') {
				return fail(p, XML_ERR_UNIMPL, "comments, "
					    "processing instructions, CDATA "
					    "and the doctype");
			} else {
				p->i += 1;
				if (!parse_stag(p, &empty))
					return 0;
			}
			if (p->stack.n == 0)
				break;	/* the root element just closed */
		} else if (byte_at(p, 0) == '&') {
			return fail(p, XML_ERR_UNIMPL, "references");
		} else if (byte_at(p, 0) < 0) {
			return fail(p, XML_ERR_SYNTAX, "input ends inside an "
				    "element");
		} else {
			if (!parse_chardata(p))
				return 0;
		}
	}
	/* Misc* : nothing but whitespace is allowed after the root for now. */
	skip_s(p);
	if (p->i != p->n)
		return fail(p, XML_ERR_SYNTAX, "content after the root "
			    "element [1]");
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
	slice s;

	if (p->n == 0)
		return fail(p, XML_ERR_NAME, "empty");
	if (!scan_name(p, &s))
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
	} else if (charmode) {
		ok = sweep_chars(&p);
		if (ok)
			printf("%lu characters\n", charcount);
	} else {
		ok = parse_document(&p);
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
	free(p.stack.v);
	free(p.att.v);
	return ok ? 0 : 1;
}
