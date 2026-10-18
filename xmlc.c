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
	XML_ERR_EXPAND,		/* entity expansion past a cap: billion laughs */
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
	"entity expansion limit exceeded",
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

/* ---------------------------------------------------------------- entities

   An internal entity is a name and a replacement text, and the text is kept
   RAW: character references and general-entity references inside it are left
   exactly as written and re-scanned when the entity is included.  That is not
   laziness, it is the only way to keep the distinction the specification
   draws between markup and data.  '<!ENTITY e "&#60;">' has to put a '<'
   CHARACTER into the content; expanding the reference at declaration time
   would store a literal '<' and the inclusion would then read it as the start
   of a tag, turning a legal document into a malformed one.

   Parameter-entity references inside an entity value ARE expanded at
   declaration time, because [9] says they are.  */
typedef struct {
	unsigned char *name;
	size_t namen;
	unsigned char *text;	/* NULL for an external entity: never fetched */
	size_t textn;
	int pe;			/* declared '<!ENTITY % name ...>' */
	int external;
	int unparsed;		/* ExternalID NDataDecl: not parsed, ever */
} entity;

/* One suspended input.  The parser reads p->b[p->i..p->n) wherever that
 * happens to point, so including an entity is a push and reaching the end of
 * its replacement text is a pop.  Entities therefore cost no C stack either,
 * same as element nesting. */
typedef struct {
	const unsigned char *b;
	size_t n, i;
	size_t eidx;		/* index into p->ent, for WFC: No Recursion */
	size_t sdepth;		/* p->stack.n when this frame was pushed */
} frame;

/* Billion laughs needs TWO caps and neither one alone would do.  The classic
 * bomb is nine entities deep and expands to a gigabyte, so a depth limit
 * never fires on it and a byte limit does.  A 100000-deep chain of one-byte
 * entities is the other way round.  Self-reference is a third thing again and
 * is a well-formedness constraint, not a cap.  All three are separate checks.
 *
 * shortcut: both numbers are compile-time constants.  Make them options if a
 * legitimate document ever wants more than 8MB of expansion. */
#define XMLC_MAX_ENTITY_DEPTH 64
#define XMLC_MAX_EXPANSION (8u * 1024u * 1024u)

/* Content models nest -- ((a|b),(c|d)) -- and that one production is parsed
 * recursively, so it gets a ceiling of its own. */
#define XMLC_MAX_MODEL_DEPTH 256

typedef struct {
	const unsigned char *b;
	size_t n, i;

	xml_err err;
	const char *detail;	/* static string, or NULL */
	size_t errpos;

	slicevec stack;		/* open element names, outermost first */
	slicevec att;		/* attribute names of the current start tag */

	entity *ent;		/* everything the internal subset declared */
	size_t nent, entcap;

	frame *fr;		/* entities currently being included */
	size_t nfr, frcap;
	size_t refpos;		/* byte offset in the document of the
				 * outermost reference, so a diagnostic from
				 * inside an entity points at a real byte */
	unsigned long expanded;	/* replacement bytes included so far */

	int has_extsubset;	/* the doctype named an ExternalID */
	int saw_pe;		/* a parameter-entity reference occurred */
	int standalone;		/* the declaration said standalone="yes" */
	int unread;		/* a declaration may exist somewhere this
				 * parser deliberately did not read */
} P;

/* How many code points the Char sweep saw.  Printed by -C so the sweep cannot
 * silently do nothing, which is the failure mode of every test harness. */
static unsigned long charcount;

static int fail(P *p, xml_err e, const char *detail)
{
	if (p->err == XML_OK) {
		p->err = e;
		p->detail = detail;
		/* Inside an entity p->i indexes the replacement text, which is
		 * not a position in the file the user named.  Point at the
		 * outermost reference instead. */
		p->errpos = p->nfr ? p->refpos : p->i;
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

/* ----------------------------------------------- the entity table and stack */

/* Returns an INDEX, not a pointer: declarations keep arriving while an
 * included parameter entity is being parsed, and the realloc that grows the
 * table would leave any pointer handed out earlier dangling.
 * shortcut: linear scan.  Suite documents declare tens of entities; a hash
 * table is the upgrade if one ever declares thousands. */
#define ENT_NONE ((size_t)-1)

static size_t ent_find(P *p, slice nm, int pe)
{
	size_t k;

	for (k = 0; k < p->nent; k++)
		if (p->ent[k].pe == pe && p->ent[k].namen == nm.n &&
		    memcmp(p->ent[k].name, nm.b, nm.n) == 0)
			return k;
	return ENT_NONE;
}

/* Takes ownership of text.  A second declaration of the same name is not an
 * error: 4.2 says the first one binds and the rest are ignored. */
static int ent_add(P *p, slice nm, int pe, unsigned char *text, size_t textn,
		   int external, int unparsed)
{
	entity *e;
	unsigned char *nmcopy;

	if (ent_find(p, nm, pe) != ENT_NONE) {
		free(text);
		return 1;
	}
	if (p->nent == p->entcap) {
		size_t c = p->entcap ? p->entcap * 2 : 16;
		entity *v = realloc(p->ent, c * sizeof *v);
		if (!v) {
			free(text);
			return 0;
		}
		p->ent = v;
		p->entcap = c;
	}
	nmcopy = malloc(nm.n ? nm.n : 1);
	if (!nmcopy) {
		free(text);
		return 0;
	}
	memcpy(nmcopy, nm.b, nm.n);
	e = &p->ent[p->nent++];
	e->name = nmcopy;
	e->namen = nm.n;
	e->text = text;
	e->textn = textn;
	e->pe = pe;
	e->external = external;
	e->unparsed = unparsed;
	return 1;
}

/* Suspend the current input and read the entity's replacement text instead.
 * refpos is where in the DOCUMENT the outermost reference sits, so that a
 * diagnostic raised three entities deep still names a byte the reader can
 * find. */
static int frame_push(P *p, size_t eidx, size_t refpos)
{
	const entity *e = &p->ent[eidx];
	frame f;
	size_t k;

	/* WFC: No Recursion.  Direct and transitive are the same test: every
	 * entity between the outermost reference and this one is on the
	 * stack, so '&a;' with a -> b -> a is caught at the second a. */
	for (k = 0; k < p->nfr; k++)
		if (p->fr[k].eidx == eidx)
			return fail(p, XML_ERR_WFC, "an entity may not "
				    "reference itself, directly or through "
				    "another entity (WFC: No Recursion) [68]");
	if (p->nfr >= XMLC_MAX_ENTITY_DEPTH)
		return fail(p, XML_ERR_EXPAND, "entity references nested "
			    "deeper than the expansion depth cap");
	p->expanded += e->textn;
	if (p->expanded > XMLC_MAX_EXPANSION)
		return fail(p, XML_ERR_EXPAND, "entity expansion produced "
			    "more replacement text than the byte cap allows");
	if (p->nfr == p->frcap) {
		size_t c = p->frcap ? p->frcap * 2 : 16;
		frame *v = realloc(p->fr, c * sizeof *v);
		if (!v)
			return fail(p, XML_ERR_MEM, NULL);
		p->fr = v;
		p->frcap = c;
	}
	f.b = p->b;
	f.n = p->n;
	f.i = p->i;
	f.eidx = eidx;
	f.sdepth = p->stack.n;
	if (p->nfr == 0)
		p->refpos = refpos;
	p->fr[p->nfr++] = f;
	p->b = e->text;
	p->n = e->textn;
	p->i = 0;
	return 1;
}

/* 4.3.2 Well-Formed Parsed Entity: an entity included in content has to match
 * the content production, which means every element it opens it also closes.
 * The element stack depth at the push is the whole check. */
static int frame_pop(P *p)
{
	frame f = p->fr[--p->nfr];

	p->b = f.b;
	p->n = f.n;
	p->i = f.i;
	if (p->stack.n != f.sdepth)
		return fail(p, XML_ERR_WFC, "an entity must contain whole "
			    "elements: it opened or closed a tag it did not "
			    "match (4.3.2 Well-Formed Parsed Entity)");
	return 1;
}

/* Pop every frame that has been read to its end, down to `base`. */
static int frames_sync(P *p, size_t base)
{
	while (p->i >= p->n && p->nfr > base)
		if (!frame_pop(p))
			return 0;
	return 1;
}

/* WFC: Entity Declared is relaxed for a non-validating processor that did not
 * read everything which could carry a declaration.  4.1 states the condition
 * exactly: the constraint binds for a document with no DTD, for a document
 * whose only DTD is an internal subset containing no parameter-entity
 * references, and for any document with standalone="yes".  Otherwise a
 * declaration this parser never saw might exist, and inventing an error for
 * it would be a claim about the document that xmlc cannot support. */
static int strict_decl(P *p)
{
	return p->standalone || (!p->has_extsubset && !p->saw_pe);
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
/* Defined below with the other reference machinery: attribute values and
 * content both contain references, and the two callers sit on either side of
 * it. */
static int parse_reference(P *p, int inatt);

static int scan_attvalue(P *p, slice *out)
{
	int q = byte_at(p, 0);
	size_t start, base = p->nfr;

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, "attribute value must be "
			    "quoted [10]");
	p->i++;
	start = p->i;
	for (;;) {
		int w;
		long c;

		if (!frames_sync(p, base))
			return 0;
		c = cur(p, &w);

		if (c == -2)
			return fail(p, XML_ERR_SYNTAX, "unterminated attribute "
				    "value [10]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		/* A quote inside an included entity is a character of the
		 * value, not the end of it.  Only the frame the opening quote
		 * came from can close it. */
		if (c == q && p->nfr == base)
			break;
		if (c == '<')
			return fail(p, XML_ERR_WFC, "'<' in an attribute value "
				    "(WFC: No < in Attribute Values); an entity "
				    "included here may not contain one either");
		if (c == '&') {
			/* The value slice stays the raw bytes of the literal:
			 * this parser checks well-formedness and does not hand
			 * anyone a decoded value, so materialising one here
			 * would produce a string nothing reads. Duplicate-
			 * attribute detection compares names, not values. */
			if (!parse_reference(p, 1))
				return 0;
			continue;
		}
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
	/* 4.3.2 Well-Formed Parsed Entity.  Matching depths at the end of the
	 * entity are not enough: '</foo><foo>' leaves the stack exactly as it
	 * found it and is still not content.  The stack may never drop BELOW
	 * where the entity started. */
	if (p->nfr && p->stack.n <= p->fr[p->nfr - 1].sdepth)
		return fail(p, XML_ERR_WFC, "an entity may not close an "
			    "element it did not open (4.3.2 Well-Formed "
			    "Parsed Entity)");
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

/* [66] CharRef   ::= '&#' [0-9]+ ';' | '&#x' [0-9a-fA-F]+ ';'
 * [68] EntityRef ::= '&' Name ';'
 * [67] Reference ::= EntityRef | CharRef
 *
 * Two well-formedness constraints live here, and neither is syntactic:
 *
 *   Legal Character   a character reference must denote a Char [2]. '&#0;'
 *                     parses perfectly and refers to a code point that may
 *                     not appear in an XML document at all.
 *   Entity Declared   a reference to an entity nobody declared is an error.
 *                     With no DTD the five predefined entities are the only
 *                     declared ones.  With an internal subset the table
 *                     decides, and 4.1 relaxes the constraint entirely when
 *                     a declaration could be sitting somewhere this parser
 *                     did not read -- see strict_decl().
 *
 * Entered with p->i on the '&'.  inatt says the reference is inside an
 * attribute value, where WFC: No External Entity References also applies. */
static int is_predef(slice n)
{
	static const char *const e[] = { "amp", "lt", "gt", "apos", "quot" };
	size_t k;

	for (k = 0; k < sizeof e / sizeof *e; k++)
		if (n.n == strlen(e[k]) && memcmp(n.b, e[k], n.n) == 0)
			return 1;
	return 0;
}

/* [66] CharRef ::= '&#' [0-9]+ ';' | '&#x' [0-9a-fA-F]+ ';'
 *
 * Entered with p->i on the '#'.  On success *out holds the code point, which
 * has already been checked against [2] Char. */
static int scan_charref(P *p, long *out)
{
	long v = 0;
	int hex = 0, digits = 0, overflow = 0;

	p->i += 1;			/* '#' */
	if (byte_at(p, 0) == 'x') {
		hex = 1;
		p->i += 1;
	}
	for (;;) {
		int c = byte_at(p, 0), d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (hex && c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (hex && c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			break;
		/* Clamped rather than wrapped: a reference to some enormous
		 * number is not a Char either way, and signed overflow would
		 * be undefined. */
		if (v <= 0x10FFFF)
			v = v * (hex ? 16 : 10) + d;
		else
			overflow = 1;
		digits++;
		p->i += 1;
	}
	if (digits == 0)
		return fail(p, XML_ERR_SYNTAX, "a character reference needs at "
			    "least one digit [66]");
	if (!eat(p, ";"))
		return fail(p, XML_ERR_SYNTAX, "expected ';' at the end of a "
			    "character reference [66]");
	if (overflow || !is_char(v))
		return fail(p, XML_ERR_WFC, "character reference to a code "
			    "point that is not a Char (WFC: Legal Character) "
			    "[66]");
	*out = v;
	return 1;
}

static int parse_reference(P *p, int inatt)
{
	slice name;
	size_t refpos = p->nfr ? p->refpos : p->i;
	size_t ei;

	p->i += 1;			/* '&' */

	if (byte_at(p, 0) == '#') {
		long cp;

		return scan_charref(p, &cp);
	}

	if (!scan_name(p, &name))
		return 0;
	if (!eat(p, ";"))
		return fail(p, XML_ERR_SYNTAX, "expected ';' at the end of an "
			    "entity reference [68]");
	/* This runs for a default attribute value in the DTD as well, and it
	 * has to.  The suite settled three questions here at once: a default
	 * may not reference an external entity (eduni rmt-e3e-12), may not
	 * reference an unparsed one (not-wf-sa-084), and may not reference an
	 * entity declared LATER in the subset (ibm68n07, not-wf-sa-180).  An
	 * earlier version of this function treated every reference inside the
	 * DTD as bypassed and syntax-checked only, which accepted all four of
	 * those documents. */
	ei = ent_find(p, name, 0);
	if (ei == ENT_NONE) {
		if (is_predef(name))
			return 1;
		if (!strict_decl(p)) {
			p->unread = 1;
			return 1;
		}
		return fail(p, XML_ERR_WFC, "reference to an entity that was "
			    "never declared; with no doctype only amp, lt, gt, "
			    "apos and quot are declared (WFC: Entity Declared) "
			    "[68]");
	}
	if (p->ent[ei].unparsed)
		return fail(p, XML_ERR_WFC, "reference to an unparsed entity, "
			    "which may only be the value of an ENTITY "
			    "attribute (WFC: Parsed Entity) [68]");
	if (p->ent[ei].external) {
		if (inatt)
			return fail(p, XML_ERR_WFC, "reference to an external "
				    "entity in an attribute value (WFC: No "
				    "External Entity References) [68]");
		/* Not fetched, by design: see the Non-goals in PLAN.md. */
		p->unread = 1;
		return 1;
	}
	return frame_push(p, ei, refpos);
}

/* [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 *
 * That production is written the long way round to say one thing: '--' may
 * not occur inside a comment, and so a comment may not end '--->'. Scanning
 * for '-->' and stopping there would accept both, which is the usual way this
 * gets implemented wrongly. Entered with p->i on the '<'. */
static int parse_comment(P *p)
{
	p->i += 4;			/* '<!--' */
	for (;;) {
		int w;
		long c;

		if (lit(p, "-->")) {
			p->i += 3;
			return 1;
		}
		c = cur(p, &w);
		if (c == -2)
			return fail(p, XML_ERR_SYNTAX,
				    "unterminated comment [15]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		/* A '-' that did not start '-->' may not be followed by
		 * another '-'. Rejects "<!-- a -- b -->" and "<!--->". */
		if (c == '-' && byte_at(p, 1) == '-')
			return fail(p, XML_ERR_SYNTAX, "'--' may not appear "
				    "inside a comment, so a comment may not "
				    "end '--->' either [15]");
		p->i += (size_t)w;
	}
}

/* [16] PI       ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
 * [17] PITarget ::= Name - (('X'|'x')('M'|'m')('L'|'l'))
 *
 * The subtraction in [17] has no syntactic cause: 'xml' is a perfectly legal
 * Name, and is excluded because the specification reserves it. */
static int parse_pi(P *p)
{
	slice target;

	p->i += 2;			/* '<?' */
	if (!scan_name(p, &target))
		return 0;
	if (target.n == 3 && (target.b[0] | 32) == 'x' &&
	    (target.b[1] | 32) == 'm' && (target.b[2] | 32) == 'l')
		return fail(p, XML_ERR_WFC, "'xml' in any combination of "
			    "cases is a reserved PI target [17]");
	if (!lit(p, "?>")) {
		if (!is_s(byte_at(p, 0)))
			return fail(p, XML_ERR_SYNTAX, "a PI target must be "
				    "followed by whitespace or '?>' [16]");
		skip_s(p);
	}
	for (;;) {
		int w;
		long c;

		if (lit(p, "?>")) {
			p->i += 2;
			return 1;
		}
		c = cur(p, &w);
		if (c == -2)
			return fail(p, XML_ERR_SYNTAX,
				    "unterminated processing instruction [16]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		p->i += (size_t)w;
	}
}

/* [18] CDSect  ::= CDStart CData CDEnd
 * [19] CDStart ::= '<![CDATA['
 * [20] CData   ::= (Char* - (Char* ']]>' Char*))
 * [21] CDEnd   ::= ']]>'
 *
 * The only place ']]>' may appear: parse_chardata rejects it everywhere else,
 * which is the other half of production [14]. */
static int parse_cdata(P *p)
{
	p->i += 9;			/* '<![CDATA[' */
	for (;;) {
		int w;
		long c;

		if (lit(p, "]]>")) {
			p->i += 3;
			return 1;
		}
		c = cur(p, &w);
		if (c == -2)
			return fail(p, XML_ERR_SYNTAX,
				    "unterminated CDATA section [18]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		p->i += (size_t)w;
	}
}

/* ============================ the document type declaration [28] =========
 *
 * Everything from here to parse_doctype is the internal subset.  Three things
 * about it are easy to get wrong and are called out where they happen:
 *
 *   - a parameter-entity reference in the internal subset may only stand
 *     where a whole markup declaration could (WFC: PEs in Internal Subset);
 *   - a conditional section may only appear in the EXTERNAL subset, so
 *     '<![INCLUDE[' here is not well-formed rather than unimplemented;
 *   - what this parser does NOT read -- the external subset, external
 *     parameter entities -- relaxes WFC: Entity Declared instead of being
 *     invented.  See strict_decl().
 */

/* A growable byte buffer, used only to build entity replacement text. */
typedef struct {
	unsigned char *b;
	size_t n, cap;
} buf;

static int buf_add(buf *o, const unsigned char *s, size_t n)
{
	if (o->n + n > o->cap) {
		size_t c = o->cap ? o->cap : 64;
		unsigned char *nb;
		while (c < o->n + n)
			c *= 2;
		nb = realloc(o->b, c);
		if (!nb)
			return 0;
		o->b = nb;
		o->cap = c;
	}
	if (n)
		memcpy(o->b + o->n, s, n);
	o->n += n;
	return 1;
}

/* [7] Nmtoken ::= (NameChar)+  -- an enumerated attribute type lists these,
 * not Names, so '1' and '-x' are legal members of an enumeration. */
static int scan_nmtoken(P *p)
{
	int any = 0;

	for (;;) {
		int w;
		long c = cur(p, &w);

		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (c < 0 || !is_namechar(c))
			break;
		p->i += (size_t)w;
		any = 1;
	}
	if (!any)
		return fail(p, XML_ERR_NAME, "expected a Nmtoken [7]");
	return 1;
}

/* [11] SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'") */
static int scan_systemliteral(P *p)
{
	int q = byte_at(p, 0);

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, "a system identifier must be "
			    "quoted [11]");
	p->i++;
	for (;;) {
		int w;
		long c = cur(p, &w);

		if (c == -2)
			return fail(p, XML_ERR_SYNTAX, "unterminated system "
				    "identifier [11]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		p->i += (size_t)w;
		if (c == q)
			return 1;
	}
}

/* [13] PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%]
 * The apostrophe is a PubidChar but cannot appear in an apostrophe-delimited
 * literal, which is the whole content of [12]'s second alternative. */
static int is_pubid(int c, int q)
{
	if (c == '\'')
		return q != '\'';
	return c == 0x20 || c == 0xD || c == 0xA ||
	    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9') ||
	    (c != 0 && c < 0x80 && strchr("-()+,./:=?;!*#@$_%", c) != NULL);
}

/* [12] PubidLiteral.  Every PubidChar is ASCII, so this reads bytes. */
static int scan_pubidliteral(P *p)
{
	int q = byte_at(p, 0);

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, "a public identifier must be "
			    "quoted [12]");
	p->i++;
	for (;;) {
		int c = byte_at(p, 0);

		if (c < 0)
			return fail(p, XML_ERR_SYNTAX, "unterminated public "
				    "identifier [12]");
		p->i++;
		if (c == q)
			return 1;
		if (!is_pubid(c, q))
			return fail(p, XML_ERR_SYNTAX, "character not allowed "
				    "in a public identifier [13]");
	}
}

/* [75] ExternalID ::= 'SYSTEM' S SystemLiteral
 *                   | 'PUBLIC' S PubidLiteral S SystemLiteral */
static int parse_externalid(P *p)
{
	if (eat(p, "SYSTEM")) {
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after SYSTEM [75]");
		return scan_systemliteral(p);
	}
	if (eat(p, "PUBLIC")) {
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after PUBLIC [75]");
		if (!scan_pubidliteral(p))
			return 0;
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "before the system identifier [75]");
		return scan_systemliteral(p);
	}
	return fail(p, XML_ERR_SYNTAX, "expected SYSTEM or PUBLIC [75]");
}

/* [69] PEReference ::= '%' Name ';' inside an entity value, appended to the
 * text being built.  The referenced entity's own text was fully expanded when
 * IT was declared, so there is nothing recursive left here -- and a parameter
 * entity cannot be written in terms of itself, because it does not enter the
 * table until its own value has been built. */
static int peref_into(P *p, buf *o)
{
	slice nm;
	size_t ei;

	p->i++;				/* '%' */
	if (!scan_name(p, &nm))
		return 0;
	if (!eat(p, ";"))
		return fail(p, XML_ERR_SYNTAX, "expected ';' at the end of a "
			    "parameter-entity reference [69]");
	p->saw_pe = 1;
	ei = ent_find(p, nm, 1);
	if (ei == ENT_NONE) {
		if (strict_decl(p))
			return fail(p, XML_ERR_WFC, "reference to a parameter "
				    "entity that was never declared (WFC: "
				    "Entity Declared) [69]");
		p->unread = 1;
		return 1;
	}
	if (p->ent[ei].external) {
		p->unread = 1;	/* never fetched, by design */
		return 1;
	}
	p->expanded += p->ent[ei].textn;
	if (p->expanded > XMLC_MAX_EXPANSION)
		return fail(p, XML_ERR_EXPAND, "parameter-entity expansion "
			    "produced more text than the byte cap allows");
	if (!buf_add(o, p->ent[ei].text, p->ent[ei].textn))
		return fail(p, XML_ERR_MEM, NULL);
	return 1;
}

/* Append one code point as UTF-8. */
static int u8_put(buf *o, long c)
{
	unsigned char t[4];
	size_t n;

	if (c < 0x80) {
		t[0] = (unsigned char)c;
		n = 1;
	} else if (c < 0x800) {
		t[0] = (unsigned char)(0xC0 | (c >> 6));
		t[1] = (unsigned char)(0x80 | (c & 0x3F));
		n = 2;
	} else if (c < 0x10000) {
		t[0] = (unsigned char)(0xE0 | (c >> 12));
		t[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
		t[2] = (unsigned char)(0x80 | (c & 0x3F));
		n = 3;
	} else {
		t[0] = (unsigned char)(0xF0 | (c >> 18));
		t[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
		t[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
		t[3] = (unsigned char)(0x80 | (c & 0x3F));
		n = 4;
	}
	return buf_add(o, t, n);
}

/* [9] EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"' | "'" ...
 *
 * Builds the replacement text, which 4.5 defines precisely: character
 * references and parameter-entity references are expanded here, general-
 * entity references are left as written and resolved when the entity is
 * included.
 *
 * The expansion of character references is not a detail.  I built this the
 * other way first -- store the literal and let the inclusion re-scan the
 * character references -- reasoning that it kept '<' out of the markup.  It
 * does, and that is the bug: 4.5 says the replacement text of
 * '<!ENTITY e "&#60;foo>">' really is '<foo>', and including it really does
 * open a tag that is never closed.  The suite rejected my version through
 * not-wf-sa-103, -090, -092 and errata-4e 140/141, six cases pointing at the
 * same wrong model.  It is also why 4.6 declares lt as '&#38;#60;' and not
 * as '&#60;': the double escape is what survives this expansion.
 *
 * A '&' that does not begin a Reference is not allowed either: [9]'s first
 * alternative is [^%&"], so the ampersand has only the one meaning. */
static int scan_entityvalue(P *p, buf *o)
{
	int q = byte_at(p, 0);

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, "an entity value must be "
			    "quoted [9]");
	p->i++;
	for (;;) {
		int w;
		long c = cur(p, &w);

		if (c == -2)
			return fail(p, XML_ERR_SYNTAX, "unterminated entity "
				    "value [9]");
		if (c == -1)
			return fail(p, XML_ERR_UTF8, NULL);
		if (!is_char(c))
			return fail(p, XML_ERR_CHAR, NULL);
		if (c == q) {
			p->i++;
			return 1;
		}
		if (c == '&') {
			if (byte_at(p, 1) == '#') {
				long cp;

				p->i++;
				if (!scan_charref(p, &cp))
					return 0;
				if (!u8_put(o, cp))
					return fail(p, XML_ERR_MEM, NULL);
			} else {
				slice nm;
				size_t start = p->i;

				p->i++;
				if (!scan_name(p, &nm))
					return 0;
				if (!eat(p, ";"))
					return fail(p, XML_ERR_SYNTAX,
						    "expected ';' at the end "
						    "of an entity reference "
						    "in an entity value [68] "
						    "[9]");
				if (!buf_add(o, p->b + start, p->i - start))
					return fail(p, XML_ERR_MEM, NULL);
			}
			continue;
		}
		if (c == '%') {
			/* WFC: PEs in Internal Subset.  Directly in the
			 * internal subset a parameter-entity reference may
			 * only replace a whole markup declaration, and an
			 * entity value is inside one.  Inside the replacement
			 * text of a parameter entity the restriction does not
			 * apply, which is why this asks the frame stack and
			 * not a flag. */
			if (p->nfr == 0)
				return fail(p, XML_ERR_WFC, "a parameter-"
					    "entity reference may not appear "
					    "inside a markup declaration in "
					    "the internal subset (WFC: PEs in "
					    "Internal Subset) [28a]");
			if (!peref_into(p, o))
				return 0;
			continue;
		}
		if (!buf_add(o, p->b + p->i, (size_t)w))
			return fail(p, XML_ERR_MEM, NULL);
		p->i += (size_t)w;
	}
}

/* [70] EntityDecl ::= GEDecl | PEDecl
 * [71] GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
 * [72] PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
 * [73] EntityDef ::= EntityValue | (ExternalID NDataDecl?)
 * [76] NDataDecl ::= S 'NDATA' S Name */
static int parse_entitydecl(P *p)
{
	slice nm, nota;
	buf o;
	int pe = 0, q;
	size_t save;

	p->i += 8;			/* '<!ENTITY' */
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "'<!ENTITY' [71]");
	if (byte_at(p, 0) == '%') {
		p->i++;
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after '%' in a parameter-entity "
				    "declaration [72]");
		pe = 1;
	}
	if (!scan_name(p, &nm))
		return 0;
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after an "
			    "entity name [71]");
	q = byte_at(p, 0);
	if (q == '"' || q == '\'') {
		memset(&o, 0, sizeof o);
		if (!scan_entityvalue(p, &o)) {
			free(o.b);
			return 0;
		}
		/* Shrink to the exact length.  Slack at the end of this
		 * allocation would let a read one byte past the replacement
		 * text land inside the same malloc block, where ASan cannot
		 * see it -- which is how a sanitizer run comes out clean over
		 * an area smaller than it looks. */
		{
			unsigned char *nb = realloc(o.b, o.n ? o.n : 1);
			if (nb)
				o.b = nb;
		}
		if (!ent_add(p, nm, pe, o.b, o.n, 0, 0))
			return fail(p, XML_ERR_MEM, NULL);
	} else {
		int unparsed = 0;

		if (!parse_externalid(p))
			return 0;
		save = p->i;
		if (skip_s(p) && eat(p, "NDATA")) {
			if (pe)
				return fail(p, XML_ERR_SYNTAX, "NDATA is not "
					    "allowed on a parameter entity "
					    "[74]");
			if (!skip_s(p))
				return fail(p, XML_ERR_SYNTAX, "expected "
					    "whitespace after NDATA [76]");
			if (!scan_name(p, &nota))
				return 0;
			unparsed = 1;
		} else {
			p->i = save;
		}
		if (!ent_add(p, nm, pe, NULL, 0, 1, unparsed))
			return fail(p, XML_ERR_MEM, NULL);
	}
	skip_s(p);
	if (!eat(p, ">"))
		return fail(p, XML_ERR_SYNTAX, "expected '>' at the end of an "
			    "entity declaration [71]");
	return 1;
}

/* [82] NotationDecl ::= '<!NOTATION' S Name S (ExternalID | PublicID) S? '>'
 * [83] PublicID ::= 'PUBLIC' S PubidLiteral
 *
 * This is the one place a public identifier may stand without a system
 * identifier after it, so ExternalID cannot simply be reused. */
static int parse_notationdecl(P *p)
{
	slice nm;
	size_t save;

	p->i += 10;			/* '<!NOTATION' */
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "'<!NOTATION' [82]");
	if (!scan_name(p, &nm))
		return 0;
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after a "
			    "notation name [82]");
	if (eat(p, "SYSTEM")) {
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after SYSTEM [75]");
		if (!scan_systemliteral(p))
			return 0;
	} else if (eat(p, "PUBLIC")) {
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after PUBLIC [83]");
		if (!scan_pubidliteral(p))
			return 0;
		save = p->i;
		if (skip_s(p) &&
		    (byte_at(p, 0) == '"' || byte_at(p, 0) == '\'')) {
			if (!scan_systemliteral(p))
				return 0;
		} else {
			p->i = save;
		}
	} else {
		return fail(p, XML_ERR_SYNTAX, "expected SYSTEM or PUBLIC in a "
			    "notation declaration [82]");
	}
	skip_s(p);
	if (!eat(p, ">"))
		return fail(p, XML_ERR_SYNTAX, "expected '>' at the end of a "
			    "notation declaration [82]");
	return 1;
}

/* [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 * [49] choice ::= '(' S? cp ( S? '|' S? cp )+ S? ')'
 * [50] seq    ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [49] and [50] differ only in their separator, and the separator may not be
 * mixed inside one group: '(a|b,c)' is not a content model. */
static int parse_cp(P *p, int depth);

static int parse_group(P *p, int depth)
{
	int sep = 0, c;

	if (depth > XMLC_MAX_MODEL_DEPTH)
		return fail(p, XML_ERR_DEPTH, "content model nested too "
			    "deeply [47]");
	if (!eat(p, "("))
		return fail(p, XML_ERR_SYNTAX, "expected '(' in a content "
			    "model [47]");
	skip_s(p);
	if (!parse_cp(p, depth + 1))
		return 0;
	for (;;) {
		skip_s(p);
		c = byte_at(p, 0);
		if (c == ')') {
			p->i++;
			return 1;
		}
		if (c != '|' && c != ',')
			return fail(p, XML_ERR_SYNTAX, "expected '|', ',' or "
				    "')' in a content model [49] [50]");
		if (sep && c != sep)
			return fail(p, XML_ERR_SYNTAX, "a content model group "
				    "is all '|' or all ',', never both [49] "
				    "[50]");
		sep = c;
		p->i++;
		skip_s(p);
		if (!parse_cp(p, depth + 1))
			return 0;
	}
}

static int parse_cp(P *p, int depth)
{
	slice nm;
	int c;

	if (byte_at(p, 0) == '(') {
		if (!parse_group(p, depth))
			return 0;
	} else if (!scan_name(p, &nm)) {
		return 0;
	}
	c = byte_at(p, 0);
	if (c == '?' || c == '*' || c == '+')
		p->i++;
	return 1;
}

/* [46] contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
 * [51] Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*'
 *              | '(' S? '#PCDATA' S? ')' */
static int parse_contentspec(P *p)
{
	size_t save;
	int c, named = 0;

	if (eat(p, "EMPTY") || eat(p, "ANY"))
		return 1;
	if (byte_at(p, 0) != '(')
		return fail(p, XML_ERR_SYNTAX, "expected EMPTY, ANY or a "
			    "content model [46]");
	save = p->i;
	p->i++;
	skip_s(p);
	if (eat(p, "#PCDATA")) {
		for (;;) {
			slice nm;

			skip_s(p);
			if (!eat(p, "|"))
				break;
			skip_s(p);
			if (!scan_name(p, &nm))
				return 0;
			named = 1;
		}
		if (!eat(p, ")"))
			return fail(p, XML_ERR_SYNTAX, "expected ')' at the "
				    "end of a mixed content model [51]");
		/* '(#PCDATA|a)' must be starred; '(#PCDATA)' need not be. */
		if (named && !eat(p, "*"))
			return fail(p, XML_ERR_SYNTAX, "a mixed content model "
				    "that names elements must end ')*' [51]");
		if (!named)
			eat(p, "*");
		return 1;
	}
	p->i = save;
	if (!parse_group(p, 0))
		return 0;
	c = byte_at(p, 0);
	if (c == '?' || c == '*' || c == '+')
		p->i++;
	return 1;
}

/* [45] elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>' */
static int parse_elementdecl(P *p)
{
	slice nm;

	p->i += 9;			/* '<!ELEMENT' */
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "'<!ELEMENT' [45]");
	if (!scan_name(p, &nm))
		return 0;
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after an "
			    "element name [45]");
	if (!parse_contentspec(p))
		return 0;
	skip_s(p);
	if (!eat(p, ">"))
		return fail(p, XML_ERR_SYNTAX, "expected '>' at the end of an "
			    "element declaration [45]");
	return 1;
}

/* [54] AttType ::= StringType | TokenizedType | EnumeratedType
 * [56] TokenizedType ::= 'ID'|'IDREF'|'IDREFS'|'ENTITY'|'ENTITIES'
 *                      | 'NMTOKEN'|'NMTOKENS'
 * [58] NotationType  ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 * [59] Enumeration   ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')' */
static int parse_atttype(P *p)
{
	/* Longest first: 'ID' is a prefix of 'IDREF' and 'IDREF' of 'IDREFS',
	 * so a shortest-first scan would accept 'IDREFS' as ID followed by
	 * rubbish.  [53] requires S after the type, which is what finally
	 * rejects 'IDX'. */
	static const char *const tok[] = {
		"CDATA", "IDREFS", "IDREF", "ID", "ENTITIES", "ENTITY",
		"NMTOKENS", "NMTOKEN"
	};
	size_t k;
	slice nm;

	for (k = 0; k < sizeof tok / sizeof *tok; k++)
		if (eat(p, tok[k]))
			return 1;
	if (eat(p, "NOTATION")) {
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after NOTATION [58]");
		if (!eat(p, "("))
			return fail(p, XML_ERR_SYNTAX, "expected '(' after "
				    "NOTATION [58]");
		do {
			skip_s(p);
			if (!scan_name(p, &nm))
				return 0;
			skip_s(p);
		} while (eat(p, "|"));
		if (!eat(p, ")"))
			return fail(p, XML_ERR_SYNTAX, "expected ')' at the "
				    "end of a notation type [58]");
		return 1;
	}
	if (byte_at(p, 0) == '(') {
		p->i++;
		do {
			skip_s(p);
			if (!scan_nmtoken(p))
				return 0;
			skip_s(p);
		} while (eat(p, "|"));
		if (!eat(p, ")"))
			return fail(p, XML_ERR_SYNTAX, "expected ')' at the "
				    "end of an enumeration [59]");
		return 1;
	}
	return fail(p, XML_ERR_SYNTAX, "expected an attribute type [54]");
}

/* [60] DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue) */
static int parse_defaultdecl(P *p)
{
	slice v;

	if (eat(p, "#REQUIRED") || eat(p, "#IMPLIED"))
		return 1;
	if (eat(p, "#FIXED") && !skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "#FIXED [60]");
	/* shortcut: the default value is parsed and discarded.  Supplying it
	 * to the document is the ATTLIST-defaults plan item, and it is only
	 * observable once there is a canonical dump to print it in. */
	return scan_attvalue(p, &v);
}

/* [52] AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
 * [53] AttDef ::= S Name S AttType S DefaultDecl */
static int parse_attlist(P *p)
{
	slice nm;

	p->i += 9;			/* '<!ATTLIST' */
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "'<!ATTLIST' [52]");
	if (!scan_name(p, &nm))
		return 0;
	for (;;) {
		int sp = skip_s(p);

		if (eat(p, ">"))
			return 1;
		if (!sp)
			return fail(p, XML_ERR_SYNTAX, "expected whitespace or "
				    "'>' in an attribute-list declaration "
				    "[52]");
		if (!scan_name(p, &nm))
			return 0;
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "after an attribute name [53]");
		if (!parse_atttype(p))
			return 0;
		if (!skip_s(p))
			return fail(p, XML_ERR_SYNTAX, "expected whitespace "
				    "before the default declaration [53]");
		if (!parse_defaultdecl(p))
			return 0;
	}
}

/* [28b] intSubset ::= (markupdecl | DeclSep)*
 * [29]  markupdecl ::= elementdecl | AttlistDecl | EntityDecl | NotationDecl
 *                    | PI | Comment
 * [28a] DeclSep ::= PEReference | S
 *
 * base is the frame depth of the document itself: a ']' seen inside an
 * included parameter entity belongs to that entity's text, not to the
 * subset. */
static int parse_intsubset(P *p, size_t base)
{
	for (;;) {
		for (;;) {
			skip_s(p);
			if (p->i < p->n || p->nfr <= base)
				break;
			if (!frame_pop(p))
				return 0;
		}
		if (p->nfr == base && byte_at(p, 0) == ']')
			return 1;
		if (p->i >= p->n)
			return fail(p, XML_ERR_SYNTAX, "unterminated internal "
				    "subset: expected ']' [28]");
		if (byte_at(p, 0) == '%') {
			slice nm;
			size_t ei, refpos = p->nfr ? p->refpos : p->i;

			p->i++;
			if (!scan_name(p, &nm))
				return 0;
			if (!eat(p, ";"))
				return fail(p, XML_ERR_SYNTAX, "expected ';' "
					    "at the end of a parameter-entity "
					    "reference [69]");
			p->saw_pe = 1;
			ei = ent_find(p, nm, 1);
			if (ei == ENT_NONE) {
				if (strict_decl(p))
					return fail(p, XML_ERR_WFC,
						    "reference to a parameter "
						    "entity that was never "
						    "declared (WFC: Entity "
						    "Declared) [69]");
				p->unread = 1;
				continue;
			}
			if (p->ent[ei].external) {
				p->unread = 1;	/* never fetched, by design */
				continue;
			}
			if (!frame_push(p, ei, refpos))
				return 0;
			continue;
		}
		if (lit(p, "<!--")) {
			if (!parse_comment(p))
				return 0;
		} else if (lit(p, "<?")) {
			if (!parse_pi(p))
				return 0;
		} else if (lit(p, "<!ELEMENT")) {
			if (!parse_elementdecl(p))
				return 0;
		} else if (lit(p, "<!ATTLIST")) {
			if (!parse_attlist(p))
				return 0;
		} else if (lit(p, "<!ENTITY")) {
			if (!parse_entitydecl(p))
				return 0;
		} else if (lit(p, "<!NOTATION")) {
			if (!parse_notationdecl(p))
				return 0;
		} else if (lit(p, "<![")) {
			/* [61] conditionalSect is part of extSubsetDecl only.
			 * In the internal subset it is not well-formed, which
			 * is a verdict about the document and not a gap in
			 * this parser. */
			return fail(p, XML_ERR_SYNTAX, "a conditional section "
				    "may only appear in the external subset "
				    "[28b] [61]");
		} else {
			return fail(p, XML_ERR_SYNTAX, "expected a markup "
				    "declaration in the internal subset [28b] "
				    "[29]");
		}
	}
}

/* [28] doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset
 *                      ']' S?)? '>'
 *
 * The external subset is noted and never fetched.  That is the non-goal in
 * PLAN.md, and the consequence is not silence: has_extsubset relaxes WFC:
 * Entity Declared, because a declaration this parser chose not to read might
 * be sitting in there. */
static int parse_doctype(P *p)
{
	slice nm;
	size_t base = p->nfr;

	p->i += 9;			/* '<!DOCTYPE' */
	if (!skip_s(p))
		return fail(p, XML_ERR_SYNTAX, "expected whitespace after "
			    "'<!DOCTYPE' [28]");
	if (!scan_name(p, &nm))
		return 0;
	if (skip_s(p) && (lit(p, "SYSTEM") || lit(p, "PUBLIC"))) {
		if (!parse_externalid(p))
			return 0;
		p->has_extsubset = 1;
		skip_s(p);
	}
	if (byte_at(p, 0) == '[') {
		p->i++;
		if (!parse_intsubset(p, base))
			return 0;
		p->i++;			/* ']' */
		skip_s(p);
	}
	if (!eat(p, ">"))
		return fail(p, XML_ERR_SYNTAX, "expected '>' at the end of the "
			    "document type declaration [28]");
	return 1;
}

/* A quoted value in the XML declaration.  Returns the contents as a slice.
 * Declaration values are not attribute values: no references, no newline
 * normalisation, and the grammar for each one is its own production. */
static int xd_quoted(P *p, slice *out, const char *what)
{
	int q = byte_at(p, 0);
	size_t start;

	if (q != '"' && q != '\'')
		return fail(p, XML_ERR_SYNTAX, what);
	p->i += 1;
	start = p->i;
	while (p->i < p->n && p->b[p->i] != q)
		p->i++;
	if (p->i >= p->n)
		return fail(p, XML_ERR_SYNTAX, "unterminated quoted value in "
			    "the XML declaration [23]");
	out->b = p->b + start;
	out->n = p->i - start;
	p->i += 1;
	return 1;
}

/* [23] XMLDecl      ::= '<?xml' VersionInfo EncodingDecl? SDDecl? '?>'
 * [24] VersionInfo  ::= S 'version' Eq ("'" VersionNum "'" | '"' VersionNum '"')
 * [26] VersionNum   ::= '1.' [0-9]+
 * [80] EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' | "'" EncName "'")
 * [81] EncName      ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
 * [32] SDDecl       ::= S 'standalone' Eq (('"' ('yes'|'no') '"') | ...)
 *
 * The order is fixed and version is mandatory, which is most of what there is
 * to get wrong.  Entered with p->i at byte 0 and '<?xml' plus whitespace
 * already confirmed by the caller. */
static int parse_xmldecl(P *p)
{
	slice v;
	size_t k;
	int had_s;

	p->i += 5;			/* '<?xml' */

	/* VersionInfo, required. */
	skip_s(p);
	if (!eat(p, "version"))
		return fail(p, XML_ERR_SYNTAX, "the XML declaration must "
			    "begin with a version [24]");
	if (!eat_eq(p))
		return 0;
	if (!xd_quoted(p, &v, "the version must be quoted [24]"))
		return 0;
	/* [26] VersionNum ::= '1.' [0-9]+ */
	if (v.n < 3 || v.b[0] != '1' || v.b[1] != '.')
		return fail(p, XML_ERR_SYNTAX, "a version number is '1.' "
			    "followed by digits [26]");
	for (k = 2; k < v.n; k++)
		if (v.b[k] < '0' || v.b[k] > '9')
			return fail(p, XML_ERR_SYNTAX, "a version number is "
				    "'1.' followed by digits [26]");
	/* An XML 1.1 document is not something this parser should judge as
	 * 1.0; the suite excludes those cases and so does this. */
	if (!(v.n == 3 && v.b[2] == '0'))
		return fail(p, XML_ERR_UNIMPL, "only XML 1.0 is implemented "
			    "[26]");

	/* EncodingDecl, optional.  Recorded and not acted on: this parser
	 * reads UTF-8 only, and a document that declares something else is
	 * not one it can judge rather than one it should reject. */
	/* [80] EncodingDecl and [32] SDDecl each BEGIN with S, so the
	 * whitespace is part of the production and not decoration between
	 * tokens. Tracking whether it was there is the difference between
	 * refusing '<?xml version="1.0"standalone="yes"?>' and accepting it,
	 * which is W3C case o-p32fail3 and which this did accept. */
	had_s = skip_s(p) > 0;

	if (had_s && lit(p, "encoding")) {
		p->i += 8;
		if (!eat_eq(p))
			return 0;
		if (!xd_quoted(p, &v, "the encoding must be quoted [80]"))
			return 0;
		if (v.n == 0 ||
		    !((v.b[0] >= 'A' && v.b[0] <= 'Z') ||
		      (v.b[0] >= 'a' && v.b[0] <= 'z')))
			return fail(p, XML_ERR_SYNTAX, "an encoding name "
				    "starts with a letter [81]");
		for (k = 1; k < v.n; k++) {
			unsigned char c = v.b[k];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			      || (c >= '0' && c <= '9') || c == '.'
			      || c == '_' || c == '-'))
				return fail(p, XML_ERR_SYNTAX, "illegal "
					    "character in an encoding name "
					    "[81]");
		}
		/* US-ASCII is a subset of UTF-8, so it is the same decoder and
		 * the contract lists both.  Declining it was a bug against
		 * this project's own contract, and the suite found it: an
		 * encoding='ASCII' document with a malformed declaration was
		 * declined instead of rejected.
		 * shortcut: an ASCII-declared document containing bytes above
		 * 0x7F is accepted as UTF-8 rather than refused.  The upgrade
		 * is a byte-range check gated on the declared name. */
		{
			static const char *const ok[] = { "utf-8", "us-ascii",
							  "ascii" };
			size_t j;
			int match = 0;

			for (j = 0; j < sizeof ok / sizeof *ok; j++) {
				size_t l = strlen(ok[j]), m;

				if (v.n != l)
					continue;
				for (m = 0; m < l; m++)
					if ((v.b[m] | 32) != (unsigned char)ok[j][m])
						break;
				if (m == l) {
					match = 1;
					break;
				}
			}
			if (!match)
				return fail(p, XML_ERR_UNIMPL, "only UTF-8 and "
					    "US-ASCII input are implemented "
					    "[80]");
		}
		had_s = skip_s(p) > 0;
	}

	/* SDDecl, optional. */
	if (had_s && lit(p, "standalone")) {
		p->i += 10;
		if (!eat_eq(p))
			return 0;
		if (!xd_quoted(p, &v, "standalone must be quoted [32]"))
			return 0;
		if (!((v.n == 3 && memcmp(v.b, "yes", 3) == 0) ||
		      (v.n == 2 && memcmp(v.b, "no", 2) == 0)))
			return fail(p, XML_ERR_SYNTAX, "standalone is 'yes' "
				    "or 'no' [32]");
		/* Not decoration: standalone="yes" is a promise that nothing
		 * outside this file affects it, which switches WFC: Entity
		 * Declared from relaxed back to binding.  See strict_decl(). */
		p->standalone = (v.n == 3);
		skip_s(p);
	}

	if (!eat(p, "?>"))
		return fail(p, XML_ERR_SYNTAX, "expected '?>' at the end of "
			    "the XML declaration [23]");
	return 1;
}

/* [1]  document ::= prolog element Misc*
 *
 * Iterative.  The element stack is on the heap, so nesting depth never
 * becomes C stack depth; a pathologically deep document is XML_ERR_DEPTH and
 * not a segfault. */
static int parse_document(P *p)
{
	int empty;
	int seen_root = 0, seen_doctype = 0;

	/* Comments and PIs are Misc, so they are legal before the root as well
	 * as inside and after it. That is why the loop tracks whether the root
	 * has been seen instead of leaving as soon as the element stack is
	 * empty: with a comment in the prolog the stack is empty and the
	 * document has not started. */
	/* [23] XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
	 *
	 * This is NOT a processing instruction, even though it opens with
	 * '<?'. It is its own production, it may appear only at byte 0 with
	 * no whitespace before it, and 'xml' is reserved as a PI target
	 * precisely so that the two can never be confused.
	 *
	 * Getting that wrong is expensive in a way worth recording: routing
	 * it through parse_pi made every document with a declaration -- which
	 * is most of them -- fail production [17] and be REJECTED as
	 * not-well-formed. The suite went from 2 should-have-caught cases to
	 * 183, all of them confident wrong answers about valid documents.
	 * Unimplemented and not-well-formed are different answers, which is
	 * the entire reason exit 3 exists here.
	 *
	 * The whitespace test is the grammar: VersionInfo begins with S, so
	 * '<?xmlfoo?>' is a PI whose target is the legal Name 'xmlfoo', and
	 * only the exact three-letter name is reserved. */
	if (lit(p, "<?xml") && is_s(byte_at(p, 5)) && !parse_xmldecl(p))
		return 0;

	skip_s(p);
	if (byte_at(p, 0) != '<')
		return fail(p, XML_ERR_SYNTAX, "a document must have a root "
			    "element [1]");
	for (;;) {
		/* An entity is finished when its replacement text is: pop
		 * back to whatever was reading before the reference.  This is
		 * the only place content-level frames go away, which is also
		 * what stops a tag, a comment or a CDATA section from
		 * starting in one entity and ending in another. */
		if (!frames_sync(p, 0))
			return 0;
		if (byte_at(p, 0) == '<') {
			if (lit(p, "<!--")) {
				if (!parse_comment(p))
					return 0;
			} else if (lit(p, "<![CDATA[")) {
				/* [43] content: a CDATA section is content,
				 * and content only exists inside an element. */
				if (p->stack.n == 0)
					return fail(p, XML_ERR_SYNTAX,
						    "a CDATA section may only "
						    "appear inside an element "
						    "[43]");
				if (!parse_cdata(p))
					return 0;
			} else if (byte_at(p, 1) == '?') {
				if (!parse_pi(p))
					return 0;
			} else if (lit(p, "<!DOCTYPE")) {
				/* [1] document ::= prolog element Misc*, and
				 * [22] prolog puts the doctype before the
				 * root.  One only, and never inside an
				 * element. */
				if (seen_root || p->stack.n)
					return fail(p, XML_ERR_SYNTAX, "the "
						    "document type declaration "
						    "may only appear before "
						    "the root element [1] "
						    "[22]");
				if (seen_doctype)
					return fail(p, XML_ERR_SYNTAX, "a "
						    "second document type "
						    "declaration [22]");
				seen_doctype = 1;
				if (!parse_doctype(p))
					return 0;
			} else if (byte_at(p, 1) == '!') {
				return fail(p, XML_ERR_SYNTAX, "expected a "
					    "comment, a CDATA section or a "
					    "document type declaration after "
					    "'<!'");
			} else if (byte_at(p, 1) == '/') {
				p->i += 2;
				if (!parse_etag(p))
					return 0;
				if (p->stack.n == 0)
					break;	/* the root just closed */
			} else {
				int root = p->stack.n == 0;

				if (root && seen_root)
					return fail(p, XML_ERR_SYNTAX,
						    "a second root element "
						    "[1]");
				p->i += 1;
				if (!parse_stag(p, &empty))
					return 0;
				if (root) {
					seen_root = 1;
					if (p->stack.n == 0)
						break;	/* empty root <a/> */
				}
			}
		} else if (byte_at(p, 0) == '&') {
			if (p->stack.n == 0)
				return fail(p, XML_ERR_SYNTAX, "a reference "
					    "may only appear inside an element "
					    "[43]");
			if (!parse_reference(p, 0))
				return 0;
		} else if (byte_at(p, 0) < 0) {
			return fail(p, XML_ERR_SYNTAX, seen_root
				    ? "input ends inside an element"
				    : "a document must have a root element "
				      "[1]");
		} else if (p->stack.n == 0) {
			/* Outside the root, Misc allows only whitespace here;
			 * character data is content and content needs an
			 * element around it. */
			if (!is_s(byte_at(p, 0)))
				return fail(p, XML_ERR_SYNTAX, seen_root
					    ? "content after the root element "
					      "[1]"
					    : "content before the root element "
					      "[1]");
			skip_s(p);
		} else {
			if (!parse_chardata(p))
				return 0;
		}
	}
	/* [27] Misc ::= Comment | PI | S  -- after the root as well. */
	for (;;) {
		skip_s(p);
		if (p->i >= p->n)
			break;
		if (lit(p, "<!--")) {
			if (!parse_comment(p))
				return 0;
		} else if (byte_at(p, 0) == '<' && byte_at(p, 1) == '?') {
			if (!parse_pi(p))
				return 0;
		} else {
			return fail(p, XML_ERR_SYNTAX, "content after the root "
				    "element [1]");
		}
	}
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
	fputs("usage: xmlc [-N|-C] [FILE]\n"
	      "  (no flag)  check well-formedness: exit 0 well-formed, 1 not,\n"
	      "             2 unreadable, 3 uses an unimplemented production\n"
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
	free(p.fr);
	{
		size_t k;
		for (k = 0; k < p.nent; k++) {
			free(p.ent[k].name);
			free(p.ent[k].text);
		}
		free(p.ent);
	}
	if (ok)
		return 0;
	/* 3, not 1, when the production is simply not written yet.
	 *
	 * "I decline to judge" and "this document is not well-formed" were the
	 * same exit status, so the scorer counted every decline as a correct
	 * rejection. The headline `SCORE not-wf 897/897 rejected` was 799
	 * declines and 98 real rejections -- 89% of it xmlc saying it does not
	 * do comments, processing instructions, CDATA or the doctype yet.
	 *
	 * PLAN.md's first commit entry warns about exactly this: "a stub that
	 * rejects every input scores 1498/1498 on not-wf, which proves that a
	 * not-wf score alone measures nothing." The score was 89% of the way
	 * to being that stub, and nothing in the output said so.
	 *
	 * The contract gains a status rather than the scorer gaining a grep of
	 * stderr: an exit status is the interface, and a caller other than the
	 * scorer deserves the same distinction. */
	return p.err == XML_ERR_UNIMPL ? 3 : 1;
}
