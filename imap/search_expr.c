/* search_expr.c -- query tree handling for SEARCH
 *
 * Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "imap_err.h"
#include "search_expr.h"
#include "message.h"
#include "charset.h"
#include "annotate.h"
#include "global.h"
#include "lsort.h"
#include "xstrlcpy.h"
#include "xmalloc.h"

#define DEBUG 0

#if DEBUG
static search_expr_t **the_rootp;
static search_expr_t *the_focus;
#endif

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

static search_expr_t *append(search_expr_t *parent, search_expr_t *child)
{
    search_expr_t **tailp;

    for (tailp = &parent->children ; *tailp ; tailp = &(*tailp)->next)
	;
    *tailp = child;
    child->next = NULL;
    child->parent = parent;

    return child;
}

static search_expr_t *detachp(search_expr_t **prevp)
{
    search_expr_t *child = *prevp;

    if (child) {
	*prevp = child->next;
	child->next = NULL;
	child->parent = NULL;
    }

    return child;
}

static search_expr_t *detach(search_expr_t *parent, search_expr_t *child)
{
    search_expr_t **prevp;

    for (prevp = &parent->children ; *prevp && *prevp != child; prevp = &(*prevp)->next)
	;
    return detachp(prevp);
}

/*
 * Detach the node '*prevp' from the tree, and reparent its children to
 * '*prevp' parent, preseving '*prevp's location and its children's
 * order.
 *
 * Apparently this operation is called "splat" but I think that's
 * a damn silly name.
 */
static search_expr_t *elide(search_expr_t **prevp)
{
    search_expr_t *e = *prevp;
    search_expr_t *child;

    *prevp = e->children;

    for (child = e->children ; child ; child = child->next) {
	child->parent = e->parent;
	prevp = &child->next;
    }
    *prevp = e->next;

    e->next = NULL;
    e->children = NULL;
    e->parent = NULL;

    return e;
}

static search_expr_t *interpolate(search_expr_t **prevp, enum search_op op)
{
    search_expr_t *e = search_expr_new(NULL, op);

    e->parent = (*prevp)->parent;
    e->children = (*prevp);
    e->next = (*prevp)->next;
    (*prevp)->next = NULL;
    (*prevp)->parent = e;
    *prevp = e;

    return e;
}

/*
 * Create a new node in a search expression tree, with the given
 * operation.  If 'parent' is not NULL, the new node is attached as the
 * last child of 'parent'.  Returns a new node, never returns NULL.
 */
EXPORTED search_expr_t *search_expr_new(search_expr_t *parent, enum search_op op)
{
    search_expr_t *e = xzmalloc(sizeof(search_expr_t));
    e->op = op;
    if (parent) append(parent, e);
    return e;
}

/*
 * Recursively free a search expression tree including the given node
 * and all descendant nodes.
 */
EXPORTED void search_expr_free(search_expr_t *e)
{
    while (e->children)
	search_expr_free(detach(e, e->children));
    if (e->attr) {
	if (e->attr->internalise) e->attr->internalise(NULL, NULL, &e->internalised);
	if (e->attr->free) e->attr->free(&e->value);
    }
    free(e);
}

/*
 * Create and return a new search expression tree which is an
 * exact duplicate of the given tree.
 */
EXPORTED search_expr_t *search_expr_duplicate(const search_expr_t *e)
{
    search_expr_t *newe;
    search_expr_t *child;

    newe = search_expr_new(NULL, e->op);
    newe->attr = e->attr;
    if (newe->attr && newe->attr->duplicate)
	newe->attr->duplicate(&newe->value, &e->value);
    else
	newe->value = e->value;

    for (child = e->children ; child ; child = child->next)
	append(newe, search_expr_duplicate(child));

    return newe;
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

static const char *op_strings[] = {
    "unknown", "true", "false",
    "lt", "le", "gt", "ge", "match",
    "and", "or", "not"
};

static const char *op_as_string(unsigned int op)
{
    return (op < VECTOR_SIZE(op_strings) ? op_strings[op] : "WTF?");
}

static void serialise(const search_expr_t *e, struct buf *buf)
{
    const search_expr_t *child;

#if DEBUG
    if (e == the_focus) buf_putc(buf, '<');
#endif
    buf_putc(buf, '(');
    buf_appendcstr(buf, op_as_string(e->op));
    if (e->attr) {
	buf_putc(buf, ' ');
	buf_appendcstr(buf, e->attr->name);
	buf_putc(buf, ' ');
	if (e->attr->serialise) e->attr->serialise(buf, &e->value);
    }
    for (child = e->children ; child ; child = child->next) {
	buf_putc(buf, ' ');
	serialise(child, buf);
    }
    buf_putc(buf, ')');
#if DEBUG
    if (e == the_focus) buf_putc(buf, '>');
#endif
}

/*
 * Given an expression tree, return a string which uniquely describes
 * the tree.  The string is designed to be used as a cache key and for
 * unit tests, not for human readability.
 *
 * Returns a new string which must be free()d by the caller.
 */
EXPORTED char *search_expr_serialise(const search_expr_t *e)
{
    struct buf buf = BUF_INITIALIZER;
    serialise(e, &buf);
    return buf_release(&buf);
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

static int getseword(struct protstream *prot, char *buf, int maxlen)
{
    int c = EOF;
    int quoted = 0;

    c = prot_getc(prot);
    if (c == '"')
	quoted = 1;
    else
	prot_ungetc(c, prot);

    while (maxlen > 1 &&
	   (c = prot_getc(prot)) != EOF &&
	   (quoted ?
	       (c != '"') :
	       (c != ' ' && c != ')'))) {
	*buf++ = c;
	maxlen--;
    }
    *buf = '\0';
    if (quoted && c != EOF)
	c = prot_getc(prot);
    return c;
}

static search_expr_t *unserialise(search_expr_t *parent,
				  struct protstream *prot)
{
    int c;
    search_expr_t *e = NULL;
    unsigned int op;
    char tmp[128];

    c = prot_getc(prot);
    if (c != '(')
	goto bad;

    c = getseword(prot, tmp, sizeof(tmp));
    if (c != ' ' && c != ')')
	goto bad;

    for (op = 0 ; op < VECTOR_SIZE(op_strings) ; op++) {
	if (!strcmp(tmp, op_strings[op]))
	    break;
    }
    if (op == VECTOR_SIZE(op_strings))
	goto bad;

    e = search_expr_new(parent, op);
    if (c == ')')
	return e;    /* SEOP_TRUE, SEOP_FALSE */

    switch (op) {
    case SEOP_AND:
    case SEOP_OR:
    case SEOP_NOT:
	/* parse children */
	for (;;) {
	    c = prot_getc(prot);
	    if (c == '(') {
		prot_ungetc(c, prot);
		if (unserialise(e, prot) == NULL)
		    goto bad;
		c = prot_getc(prot);
		if (c == ')')
		    break;
		if (c != ' ')
		    goto bad;
	    }
	}
	break;
    case SEOP_LT:
    case SEOP_LE:
    case SEOP_GT:
    case SEOP_GE:
    case SEOP_MATCH:
	/* parse attribute */
	c = getseword(prot, tmp, sizeof(tmp));
	if (c != ' ')
	    goto bad;
	e->attr = search_attr_find(tmp);
	if (e->attr == NULL)
	    goto bad;
	/* parse value */
	if (e->attr->unserialise)
	    c = e->attr->unserialise(prot, &e->value);
	if (c != ')')
	    goto bad;
	break;
    default:
	c = prot_getc(prot);
	if (c != ')')
	    goto bad;
	break;
    }

    return e;

bad:
    if (e) {
	e->op = SEOP_UNKNOWN;
	if (parent == NULL)
	    search_expr_free(e);
    }
    return NULL;
}

/*
 * Given a string generated by search_expr_serialise(),
 * parse it and return a new expression tree, or NULL if
 * there were any errors.  Used mainly for unit tests.
 */
EXPORTED search_expr_t *search_expr_unserialise(const char *s)
{
    struct protstream *prot;
    search_expr_t *root = NULL;

    if (!s || !*s) return NULL;
    prot = prot_readmap(s, strlen(s));
    root = unserialise(NULL, prot);

#if DEBUG
    if (!root) {
#define MAX_CONTEXT 48
	int off = ((const char *)prot->ptr - s);
	int len = strlen(s);
	int context_begin = off - MIN(off, MAX_CONTEXT);
	int context_end = off + MIN((len-off), MAX_CONTEXT);
	int i;
	fputc('\n', stderr);
	fprintf(stderr, "ERROR: failed to unserialise string at or near:\n");
	if (context_begin) fputs("...", stderr);
	fwrite(s+context_begin, 1, context_end-context_begin, stderr);
	fputc('\n', stderr);
	if (context_begin) fputs("---", stderr);
	for (i = off - context_begin - 1 ; i > 0 ; i--)
	    fputc('-', stderr);
	fputc('^', stderr);
	fputc('\n', stderr);
    }
#endif

    prot_free(prot);
    return root;
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

enum {
    DNF_OR, DNF_AND, DNF_NOT, DNF_CMP
};

/* expected depth, in a full tree.  0 is rootmost, 3 is leafmost */
static int dnf_depth(const search_expr_t *e)
{
    switch (e->op) {
    case SEOP_TRUE:
    case SEOP_FALSE:
    case SEOP_LT:
    case SEOP_LE:
    case SEOP_GT:
    case SEOP_GE:
    case SEOP_MATCH:
	return DNF_CMP;
    case SEOP_AND:
	return DNF_AND;
    case SEOP_OR:
	return DNF_OR;
    case SEOP_NOT:
	return DNF_NOT;
    default: assert(0); return -1;
    }
    return -1;
}

static int has_enough_children(const search_expr_t *e)
{
    const search_expr_t *child;
    int min;
    int n = 0;

    switch (e->op) {
    case SEOP_OR:
    case SEOP_AND:
	min = 2;
	break;
    case SEOP_NOT:
	min = 1;
	break;
    default:
	return 1;
    }

    for (child = e->children ; child ; child = child->next)
	if (++n >= min) return 1;
    return 0;
}

static void apply_demorgan(search_expr_t **ep, search_expr_t **prevp)
{
    search_expr_t *child = *prevp;
    search_expr_t **grandp;

    /* NOT nodes have exactly one child */
    assert(*prevp != NULL);
    assert((*prevp)->next == NULL);

    child->op = (child->op == SEOP_AND ? SEOP_OR : SEOP_AND);
    for (grandp = &child->children ; *grandp ; grandp = &(*grandp)->next)
	interpolate(grandp, SEOP_NOT);
    search_expr_free(elide(ep));
}

static void apply_distribution(search_expr_t **ep, search_expr_t **prevp)
{
    search_expr_t *newor;
    search_expr_t *or;
    search_expr_t *and;
    search_expr_t *orchild;
    search_expr_t *newand;

    newor = interpolate(ep, SEOP_OR);
    and = detachp(&newor->children);
    or = detachp(prevp);

    while ((orchild = detachp(&or->children)) != NULL) {
	newand = search_expr_duplicate(and);
	append(newand, orchild);
	append(newor, newand);
    }

    search_expr_free(and);
    search_expr_free(or);
}

static void invert(search_expr_t **ep, search_expr_t **prevp)
{
    if ((*ep)->op == SEOP_NOT)
	apply_demorgan(ep, prevp);
    else
	apply_distribution(ep, prevp);
}

/* combine compatible boolean parent and child nodes */
static void combine(search_expr_t **ep, search_expr_t **prevp)
{
    switch ((*ep)->op) {
    case SEOP_NOT:
	search_expr_free(elide(prevp));
	search_expr_free(elide(ep));
	break;
    case SEOP_AND:
    case SEOP_OR:
	search_expr_free(elide(prevp));
	break;
    default:
	break;
    }
}

static int normalise(search_expr_t **ep)
{
    search_expr_t **prevp;
    int depth;
    int changed = -1;

restart:
    changed++;

#if DEBUG
    the_focus = *ep;
    {
	char *s = search_expr_serialise(*the_rootp);
	fprintf(stderr, "normalise: tree=%s\n", s);
	free(s);
    }
#endif

    if (!has_enough_children(*ep)) {
	/* eliminate trivial nodes: AND and ORs with
	 * a single child, NOTs with none */
	search_expr_free(elide(ep));
	goto restart;
    }

    depth = dnf_depth(*ep);
    for (prevp = &(*ep)->children ; *prevp ; prevp = &(*prevp)->next)
    {
	int child_depth = dnf_depth(*prevp);
	if (child_depth == depth) {
	    combine(ep, prevp);
	    goto restart;
	}
	if (child_depth < depth) {
	    invert(ep, prevp);
	    goto restart;
	}
	if (normalise(prevp))
	    goto restart;
    }

    return changed;
}

static void *getnext(void *p)
{
    return ((search_expr_t *)p)->next;
}

static void setnext(void *p, void *next)
{
    ((search_expr_t *)p)->next = next;
}

static int compare(void *p1, void *p2, void *calldata)
{
    const search_expr_t *e1 = p1;
    const search_expr_t *e2 = p2;
    int r;

    r = dnf_depth(e2) - dnf_depth(e1);

    if (!r)
	r = strcasecmp(e1->attr ? e1->attr->name : "zzz",
		       e2->attr ? e2->attr->name : "zzz");

    if (!r)
	r = (int)e1->op - (int)e2->op;

    if (!r) {
	struct buf b1 = BUF_INITIALIZER;
	struct buf b2 = BUF_INITIALIZER;
	if (e1->attr && e1->attr->serialise)
	    e1->attr->serialise(&b1, &e1->value);
	if (e2->attr && e2->attr->serialise)
	    e2->attr->serialise(&b2, &e2->value);
	r = strcmp(buf_cstring(&b1), buf_cstring(&b2));
	buf_free(&b1);
	buf_free(&b2);
    }

    if (!r) {
	if (e1->children || e2->children)
	    r = compare((void *)(e1->children ? e1->children : e1),
		        (void *)(e2->children ? e2->children : e2),
			calldata);
    }

    return r;
}

static void sort_children(search_expr_t *e)
{
    search_expr_t *child;

    for (child = e->children ; child ; child = child->next)
	sort_children(child);

    e->children = lsort(e->children, getnext, setnext, compare, NULL);
}

/*
 * Reorganise a search expression tree into Disjunctive Normal Form.
 * This form is useful for picking out cacheable and runnable sub-queries.
 *
 * An expression in DNF has a number of constraints:
 *
 * - it contains at most one OR node
 * - if present the OR node is the root
 * - NOT nodes if present have only comparisons as children
 * - it contains at most 4 levels of nodes
 * - nodes have a strict order of types, down from the root
 *   they are: OR, AND, NOT, comparisons.
 *
 * DNF is useful for running queries.  Each of the children of the
 * root OR node can be run as a separate sub-query, and cached
 * independently because their results are just accumulated together
 * without any further processing.  Each of those children is a single
 * conjuctive clause which can implemented using an index lookup (or a
 * scan of all messages) followed by a filtering step.  Finally, each of
 * those conjunctive clauses can be analysed to discover which folders
 * will need to be opened: no folders, a single specific folder,
 * all folders, or all folders except some specific folders.
 *
 * We also enforce a fixed order on child nodes of any node, so
 * that all logically equivalent trees are the same shape.  This
 * helps when constructing a cache key from a tree.  The sorting
 * criteria are:
 *
 * - NOT nodes after un-negated comparison nodes, then
 * - comparison nodes sorted lexically on attribute, then
 * - comparison nodes sorted lexically on stringified value
 *
 * Note that IMAP search syntax, when translated most directly into an
 * expression tree, defines trees whose outermost node is always an AND.
 * Those trees are not in any kind of normal form but more closely
 * resemble Conjunctive Normal Form than DNF.  Any IMAP search program
 * containing an OR criterion will require significant juggling to
 * achieve DNF.
 *
 * Takes the root of the tree in *'ep' and returns a possibly reshaped
 * tree whose root is stored in *'ep'.
 */
EXPORTED void search_expr_normalise(search_expr_t **ep)
{
#if DEBUG
    the_rootp = ep;
#endif
    normalise(ep);
    sort_children(*ep);
#if DEBUG
    the_rootp = NULL;
    the_focus = NULL;
#endif
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

/*
 * Prepare the given expression for use with the given mailbox.
 */
EXPORTED void search_expr_internalise(struct mailbox *mailbox, search_expr_t *e)
{
    search_expr_t *child;

    if (e->attr && e->attr->internalise)
	e->attr->internalise(mailbox, &e->value, &e->internalised);

    for (child = e->children ; child ; child = child->next)
	search_expr_internalise(mailbox, child);
}

/*
 * Evaluate the given search expression for the given message,
 * Returns nonzero if the expression is true, 0 otherwise.
 */
EXPORTED int search_expr_evaluate(message_t *m, const search_expr_t *e)
{
    search_expr_t *child;

    switch (e->op) {
    case SEOP_UNKNOWN: assert(0); return 1;
    case SEOP_TRUE: return 1;
    case SEOP_FALSE: return 0;
    case SEOP_LT:
	assert(e->attr);
	assert(e->attr->cmp);
	return (e->attr->cmp(m, &e->value, e->internalised, e->attr->data1) < 0);
    case SEOP_LE:
	assert(e->attr);
	assert(e->attr->cmp);
	return (e->attr->cmp(m, &e->value, e->internalised, e->attr->data1) <= 0);
    case SEOP_GT:
	assert(e->attr);
	assert(e->attr->cmp);
	return (e->attr->cmp(m, &e->value, e->internalised, e->attr->data1) > 0);
    case SEOP_GE:
	assert(e->attr);
	assert(e->attr->cmp);
	return (e->attr->cmp(m, &e->value, e->internalised, e->attr->data1) >= 0);
    case SEOP_MATCH:
	assert(e->attr);
	assert(e->attr->match);
	return e->attr->match(m, &e->value, e->internalised, e->attr->data1);
    case SEOP_AND:
	for (child = e->children ; child ; child = child->next)
	    if (!search_expr_evaluate(m, child))
		return 0;
	return 1;
    case SEOP_OR:
	for (child = e->children ; child ; child = child->next)
	    if (search_expr_evaluate(m, child))
		return 1;
	return 0;
    case SEOP_NOT:
	assert(e->children);
	return !search_expr_evaluate(m, e->children);
    }
    return 0;
}

/* ====================================================================== */

static int uses_attr(const search_expr_t *e, const search_attr_t *attr)
{
    const search_expr_t *child;

    if (e->attr == attr)
	return 1;

    for (child = e->children ; child ; child = child->next)
	if (uses_attr(child, attr))
	    return 1;

    return 0;
}

/*
 * Returns non-zero if any comparison node in the given search
 * expression tree uses the attribute with the given name.
 */
EXPORTED int search_expr_uses_attr(const search_expr_t *e, const char *name)
{
    const search_attr_t *attr = search_attr_find(name);

    if (!name) return 0;
    return uses_attr(e, attr);
}

/* ====================================================================== */

static int search_string_match(message_t *m, const union search_value *v,
				void *internalised, void *data1)
{
    int r;
    struct buf buf = BUF_INITIALIZER;
    int (*getter)(message_t *, struct buf *) = (int(*)(message_t *, struct buf *))data1;
    comp_pat *pat = (comp_pat *)internalised;

    r = getter(m, &buf);
    if (!r)
	r = charset_searchstring(v->s, pat, buf.s, buf.len, charset_flags);
    else
	r = 0;
    buf_free(&buf);

    return r;
}

static void search_string_serialise(struct buf *b, const union search_value *v)
{
    buf_printf(b, "\"%s\"", v->s);
}

static int search_string_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[1024];

    c = getseword(prot, tmp, sizeof(tmp));
    v->s = xstrdup(tmp);
    return c;
}

static void search_string_internalise(struct mailbox *mailbox __attribute__((unused)),
				      const union search_value *v, void **internalisedp)
{
    if (*internalisedp) {
	charset_freepat(*internalisedp);
	*internalisedp = NULL;
    }
    if (v) {
	*internalisedp = charset_compilepat(v->s);
    }
}

static void search_string_duplicate(union search_value *new,
				    const union search_value *old)
{
    new->s = xstrdup(old->s);
}

static void search_string_free(union search_value *v)
{
    free(v->s);
    v->s = NULL;
}

/* ====================================================================== */

static int search_listid_match(message_t *m, const union search_value *v,
			       void *internalised,
			       void *data1 __attribute__((unused)))
{
    int r;
    struct buf buf = BUF_INITIALIZER;
    comp_pat *pat = (comp_pat *)internalised;

    r = message_get_listid(m, &buf);
    if (!r) {
	r = charset_searchstring(v->s, pat, buf.s, buf.len, charset_flags);
	if (r) goto out;    // success
    }

    r = message_get_mailinglist(m, &buf);
    if (!r) {
	r = charset_searchstring(v->s, pat, buf.s, buf.len, charset_flags);
	if (r) goto out;    // success
    }

    r = 0;  // failure

out:
    buf_free(&buf);
    return r;
}

/* ====================================================================== */

static int search_contenttype_match(message_t *m, const union search_value *v,
				    void *internalised,
				    void *data1 __attribute__((unused)))
{
    int r;
    comp_pat *pat = (comp_pat *)internalised;
    strarray_t types = STRARRAY_INITIALIZER;
    int i;
    char combined[128];

    if (!message_get_leaf_types(m, &types)) {
	for (i = 0 ; i < types.count ; i+= 2) {
	    const char *type = types.data[i];
	    const char *subtype = types.data[i+1];

	    /* match against type */
	    r = charset_searchstring(v->s, pat, type, strlen(type), charset_flags);
	    if (r) goto out;	// success

	    /* match against subtype */
	    r = charset_searchstring(v->s, pat, subtype, strlen(subtype), charset_flags);
	    if (r) goto out;	// success

	    /* match against combined type_subtype */
	    snprintf(combined, sizeof(combined), "%s_%s", type, subtype);
	    r = charset_searchstring(v->s, pat, combined, strlen(combined), charset_flags);
	    if (r) goto out;	// success
	}
    }

    r = 0;  // failure

out:
    strarray_fini(&types);
    return r;
}

/* ====================================================================== */

static int search_header_match(message_t *m, const union search_value *v,
			       void *internalised, void *data1)
{
    int r;
    struct buf buf = BUF_INITIALIZER;
    const char *field = (const char *)data1;
    comp_pat *pat = (comp_pat *)internalised;

    r = message_get_field(m, field, MESSAGE_DECODED, &buf);
    if (!r) {
	r = charset_searchstring(v->s, pat, buf.s, buf.len, charset_flags);
    }
    else
	r = 0;
    buf_free(&buf);

    return r;
}

/* ====================================================================== */

static int search_seq_match(message_t *m, const union search_value *v,
			    void *internalised __attribute__((unused)),
			    void *data1)
{
    int r;
    uint32_t u;
    int (*getter)(message_t *, uint32_t *) = (int(*)(message_t *, uint32_t *))data1;

    r = getter(m, &u);
    if (!r)
	r = seqset_ismember(v->seq, u);
    else
	r = 0;

    return r;
}

static void search_seq_serialise(struct buf *b, const union search_value *v)
{
    char *ss = seqset_cstring(v->seq);
    buf_appendcstr(b, ss);
    free(ss);
}

static int search_seq_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[1024];

    c = getseword(prot, tmp, sizeof(tmp));
    v->seq = seqset_parse(tmp, NULL, /*maxval*/0);
    return c;
}

static void search_seq_duplicate(union search_value *new,
				 const union search_value *old)
{
    new->seq = seqset_dup(old->seq);
}

static void search_seq_free(union search_value *v)
{
    seqset_free(v->seq);
    v->seq = NULL;
}

/* ====================================================================== */

static int search_flags_match(message_t *m, const union search_value *v,
			      void *internalised __attribute__((unused)),
			      void *data1)
{
    int r;
    uint32_t u;
    int (*getter)(message_t *, uint32_t *) = (int(*)(message_t *, uint32_t *))data1;

    r = getter(m, &u);
    if (!r)
	r = !!(v->u & u);
    else
	r = 0;

    return r;
}

static void search_systemflags_serialise(struct buf *b, const union search_value *v)
{
    if ((v->u & FLAG_ANSWERED))
	buf_appendcstr(b, "\\Answered");
    if ((v->u & FLAG_FLAGGED))
	buf_appendcstr(b, "\\Flagged");
    if ((v->u & FLAG_DELETED))
	buf_appendcstr(b, "\\Deleted");
    if ((v->u & FLAG_DRAFT))
	buf_appendcstr(b, "\\Draft");
    if ((v->u & FLAG_SEEN))
	buf_appendcstr(b, "\\Seen");
}

static int search_systemflags_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[64];

    c = getseword(prot, tmp, sizeof(tmp));

    if (!strcasecmp(tmp, "\\Answered"))
	v->u = FLAG_ANSWERED;
    else if (!strcasecmp(tmp, "\\Flagged"))
	v->u = FLAG_FLAGGED;
    else if (!strcasecmp(tmp, "\\Deleted"))
	v->u = FLAG_DELETED;
    else if (!strcasecmp(tmp, "\\Draft"))
	v->u = FLAG_DRAFT;
    else if (!strcasecmp(tmp, "\\Seen"))
	v->u = FLAG_SEEN;
    else
	return EOF;
    return c;
}

static void search_indexflags_serialise(struct buf *b, const union search_value *v)
{
    if ((v->u & MESSAGE_SEEN))
	buf_appendcstr(b, "\\Seen");
    if ((v->u & MESSAGE_RECENT))
	buf_appendcstr(b, "\\Recent");
}

static int search_indexflags_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[64];

    c = getseword(prot, tmp, sizeof(tmp));

    if (!strcasecmp(tmp, "\\Seen"))
	v->u = MESSAGE_SEEN;
    else if (!strcasecmp(tmp, "\\Recent"))
	v->u = MESSAGE_RECENT;
    else
	return EOF;
    return c;
}

/* ====================================================================== */

static void search_keyword_internalise(struct mailbox *mailbox,
				       const union search_value *v,
				       void **internalisedp)
{
    int r;
    int num = 0;

    if (mailbox) {
	r = mailbox_user_flag(mailbox, v->s, &num, /*create*/0);
	if (!r)
	    num++;
	else
	    num = 0;
    }
    *internalisedp = (void*)(unsigned long)num;
}

static int search_keyword_match(message_t *m,
				const union search_value *v __attribute__((unused)),
				void *internalised,
				void *data1 __attribute__((unused)))
{
    int r;
    int num = (int)(unsigned long)internalised;
    uint32_t flags[MAX_USER_FLAGS/32];

    if (!num)
	return 0;   /* not a valid flag for this mailbox */
    num--;

    r = message_get_userflags(m, flags);
    if (!r)
	r = !!(flags[num/32] & (1<<(num % 32)));
    else
	r = 0;

    return r;
}

/* ====================================================================== */

static int search_uint64_match(message_t *m, const union search_value *v,
			       void *internalised __attribute__((unused)),
			       void *data1)
{
    int r;
    uint64_t u;
    int (*getter)(message_t *, uint64_t *) = (int(*)(message_t *, uint64_t *))data1;

    r = getter(m, &u);
    if (!r)
	r = (v->u == u);
    else
	r = 0;

    return r;
}

static void search_uint64_serialise(struct buf *b, const union search_value *v)
{
    buf_printf(b, "%llu", (unsigned long long)v->u);
}

static int search_uint64_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[32];

    c = getseword(prot, tmp, sizeof(tmp));
    v->u = strtoull(tmp, NULL, 10);
    return c;
}

/* ====================================================================== */

static void search_cid_serialise(struct buf *b, const union search_value *v)
{
    buf_appendcstr(b, conversation_id_encode(v->u));
}

static int search_cid_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    conversation_id_t cid;
    char tmp[32];

    c = getseword(prot, tmp, sizeof(tmp));
    if (!conversation_id_decode(&cid, tmp))
	return EOF;
    v->u = cid;
    return c;
}

/* ====================================================================== */

static void search_folder_internalise(struct mailbox *mailbox,
				      const union search_value *v,
				      void **internalisedp)
{
    if (mailbox)
	*internalisedp = (void *)(unsigned long)(!strcmp(mailbox->name, v->s));
}

static int search_folder_match(message_t *m __attribute__((unused)),
			       const union search_value *v __attribute__((unused)),
			       void *internalised, void *data1 __attribute__((unused)))
{
    return (int)(unsigned long)internalised;
}

/* ====================================================================== */

static void search_annotation_internalise(struct mailbox *mailbox,
					  const union search_value *v __attribute__((unused)),
					  void **internalisedp)
{
    *internalisedp = mailbox;
}

struct search_annot_rock {
    int result;
    const struct buf *match;
};

static int _search_annot_match(const struct buf *match,
			       const struct buf *value)
{
    /* These cases are not explicitly defined in RFC5257 */

    /* NIL matches NIL and nothing else */
    if (match->s == NULL)
	return (value->s == NULL);
    if (value->s == NULL)
	return 0;

    /* empty matches empty and nothing else */
    if (match->len == 0)
	return (value->len == 0);
    if (value->len == 0)
	return 0;

    /* RFC5257 seems to define a simple CONTAINS style search */
    return !!memmem(value->s, value->len,
		    match->s, match->len);
}

static void _search_annot_callback(const char *mboxname __attribute__((unused)),
				   uint32_t uid __attribute__((unused)),
				   const char *entry __attribute__((unused)),
				   struct attvaluelist *attvalues, void *rock)
{
    struct search_annot_rock *sarock = rock;
    struct attvaluelist *l;

    for (l = attvalues ; l ; l = l->next) {
	if (_search_annot_match(sarock->match, &l->value))
	    sarock->result = 1;
    }
}

static int search_annotation_match(message_t *m, const union search_value *v,
				   void *internalised, void *data1 __attribute__((unused)))
{
    struct mailbox *mailbox = (struct mailbox *)internalised;
    struct searchannot *sa = v->annot;
    strarray_t entries = STRARRAY_INITIALIZER;
    strarray_t attribs = STRARRAY_INITIALIZER;
    annotate_state_t *astate = NULL;
    struct search_annot_rock rock;
    uint32_t uid;
    int r;

    strarray_append(&entries, sa->entry);
    strarray_append(&attribs, sa->attrib);

    message_get_uid(m, &uid);

    r = mailbox_get_annotate_state(mailbox, uid, &astate);
    if (r) goto out;
    annotate_state_set_auth(astate, sa->isadmin,
			    sa->userid, sa->auth_state);

    memset(&rock, 0, sizeof(rock));
    rock.match = &sa->value;

    r = annotate_state_fetch(astate,
			     &entries, &attribs,
			     _search_annot_callback, &rock,
			     0);
    if (r >= 0)
	r = rock.result;

out:
    strarray_fini(&entries);
    strarray_fini(&attribs);
    return r;
}

static void search_annotation_serialise(struct buf *b, const union search_value *v)
{
    buf_printf(b, "(entry \"%s\" attrib \"%s\" value \"%s\")",
		v->annot->entry, v->annot->attrib, buf_cstring(&v->annot->value));
}

/* Note: this won't be usable for execution as it lacks
 * namespace etc pointers.  Nor can it handle binary values. */
static int search_annotation_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[64];
    char entry[1024];
    char attrib[1024];
    char value[1024];

    c = prot_getc(prot);
    if (c != '(') return EOF;

    c = getseword(prot, tmp, sizeof(tmp));
    if (c != ' ') return EOF;
    if (strcmp(tmp, "entry")) return EOF;
    c = getseword(prot, entry, sizeof(entry));
    if (c != ' ') return EOF;

    c = getseword(prot, tmp, sizeof(tmp));
    if (c != ' ') return EOF;
    if (strcmp(tmp, "attrib")) return EOF;
    c = getseword(prot, attrib, sizeof(attrib));
    if (c != ' ') return EOF;

    c = getseword(prot, tmp, sizeof(tmp));
    if (c != ' ') return EOF;
    if (strcmp(tmp, "value")) return EOF;
    c = getseword(prot, value, sizeof(value));
    if (c != ')') return EOF;

    v->annot = (struct searchannot *)xzmalloc(sizeof(struct searchannot));
    v->annot->entry = xstrdup(entry);
    v->annot->attrib = xstrdup(attrib);
    buf_appendcstr(&v->annot->value, value);
    buf_cstring(&v->annot->value);

    c = prot_getc(prot);
    return c;
}

static void search_annotation_duplicate(union search_value *new,
					const union search_value *old)
{
    new->annot = (struct searchannot *)xmemdup(old->annot, sizeof(*old->annot));
    new->annot->entry = xstrdup(new->annot->entry);
    new->annot->attrib = xstrdup(new->annot->attrib);
    buf_init(&new->annot->value);
    buf_append(&new->annot->value, &old->annot->value);
}

static void search_annotation_free(union search_value *v)
{
    if (v->annot) {
	free(v->annot->entry);
	free(v->annot->attrib);
	buf_free(&v->annot->value);
	free(v->annot);
	v->annot = NULL;
    }
}

/* ====================================================================== */

struct convflags_rock {
    struct conversations_state *cstate;
    int cstate_is_ours;
    int num;	    /* -1=invalid, 0=\Seen, 1+=index into cstate->counted_flags+1 */
};

static void search_convflags_internalise(struct mailbox *mailbox,
					 const union search_value *v,
					 void **internalisedp)
{
    struct convflags_rock *rock;
    int r;

    if (*internalisedp) {
	rock = (struct convflags_rock *)(*internalisedp);
	if (rock->cstate_is_ours)
	    conversations_abort(&rock->cstate);
	free(rock);
    }

    if (mailbox) {
	rock = xzmalloc(sizeof(struct convflags_rock));

	rock->cstate = conversations_get_mbox(mailbox->name);
	if (!rock->cstate) {
	    r = conversations_open_mbox(mailbox->name, &rock->cstate);
	    if (r)
		rock->num = -1;	    /* invalid */
	    else
		rock->cstate_is_ours = 1;
	}

	if (rock->cstate) {
	    if (!strcasecmp(v->s, "\\Seen"))
		rock->num = 0;
	    else {
		rock->num = strarray_find_case(rock->cstate->counted_flags, v->s, 0);
		/* rock->num might be -1 invalid */
		if (rock->num >= 0)
		    rock->num++;
	    }
	}

	*internalisedp = rock;
    }
}

static int search_convflags_match(message_t *m,
				  const union search_value *v __attribute__((unused)),
				  void *internalised,
				  void *data1 __attribute__((unused)))
{
    struct convflags_rock *rock = (struct convflags_rock *)internalised;
    conversation_id_t cid = NULLCONVERSATION;
    conversation_t *conv = NULL;
    int r;

    if (!rock->cstate) return 0;

    message_get_cid(m, &cid);
    if (conversation_load(rock->cstate, cid, &conv)) return 0;
    if (!conv) return 0;

    if (rock->num < 0)
	r = 0;	    /* invalid flag name */
    else if (rock->num == 0)
	r = !conv->unseen;
    else if (rock->num > 0)
	r = !!conv->counts[rock->num-1];

    conversation_free(conv);
    return r;
}

/* ====================================================================== */

/* TODO: share this code with the convflags above */
struct convmodseq_rock {
    struct conversations_state *cstate;
    int cstate_is_ours;
};

static void search_convmodseq_internalise(struct mailbox *mailbox,
					  const union search_value *v __attribute__((unused)),
					  void **internalisedp)
{
    struct convmodseq_rock *rock;
    int r;

    if (*internalisedp) {
	rock = (struct convmodseq_rock *)(*internalisedp);
	if (rock->cstate_is_ours)
	    conversations_abort(&rock->cstate);
	free(rock);
    }

    if (mailbox) {
	rock = xzmalloc(sizeof(struct convmodseq_rock));

	rock->cstate = conversations_get_mbox(mailbox->name);
	if (!rock->cstate) {
	    r = conversations_open_mbox(mailbox->name, &rock->cstate);
	    if (r)
		rock->cstate = NULL;
	    else
		rock->cstate_is_ours = 1;
	}

	*internalisedp = rock;
    }
}

static int search_convmodseq_match(message_t *m, const union search_value *v,
				   void *internalised, void *data1 __attribute__((unused)))
{
    struct convmodseq_rock *rock = (struct convmodseq_rock *)internalised;
    conversation_id_t cid = NULLCONVERSATION;
    conversation_t *conv = NULL;
    int r;

    if (!rock->cstate) return 0;

    message_get_cid(m, &cid);
    if (conversation_load(rock->cstate, cid, &conv)) return 0;
    if (!conv) return 0;

    r = (v->u == conv->modseq);

    conversation_free(conv);
    return r;
}

/* ====================================================================== */

static int search_uint32_cmp(message_t *m, const union search_value *v,
			     void *internalised __attribute__((unused)),
			     void *data1)
{
    int r;
    uint32_t u;
    int (*getter)(message_t *, uint32_t *) = (int(*)(message_t *, uint32_t *))data1;

    r = getter(m, &u);
    if (!r) {
	if (u < v->u)
	    r = -1;
	else if (u == v->u)
	    r = 0;
	else
	    r = 1;
    }
    else
	r = 0;
    return r;
}

static int search_uint32_match(message_t *m, const union search_value *v,
			       void *internalised __attribute__((unused)),
			       void *data1)
{
    int r;
    uint32_t u;
    int (*getter)(message_t *, uint32_t *) = (int(*)(message_t *, uint32_t *))data1;

    r = getter(m, &u);
    if (!r)
	r = (v->u == u);
    else
	r = 0;
    return r;
}

static void search_uint32_serialise(struct buf *b, const union search_value *v)
{
    buf_printf(b, "%u", (uint32_t)v->u);
}

static int search_uint32_unserialise(struct protstream *prot, union search_value *v)
{
    int c;
    char tmp[32];

    c = getseword(prot, tmp, sizeof(tmp));
    v->u = strtoul(tmp, NULL, 10);
    return c;
}

/* ====================================================================== */

/*
 * Search part of a message for a substring.
 */

struct searchmsg_rock
{
    const char *substr;
    comp_pat *pat;
    int skipheader;
};

static int searchmsg_cb(int partno, int charset, int encoding,
			const char *subtype __attribute((unused)),
			struct buf *data, void *rock)
{
    struct searchmsg_rock *sr = (struct searchmsg_rock *)rock;

    if (!partno) {
	/* header-like */
	if (sr->skipheader) {
	    sr->skipheader = 0; /* Only skip top-level message header */
	    return 0;
	}
	return charset_search_mimeheader(sr->substr, sr->pat,
					 buf_cstring(data), charset_flags);
    }
    else {
	/* body-like */
	if (charset < 0 || charset == 0xffff)
		return 0;
	return charset_searchfile(sr->substr, sr->pat,
				  data->s, data->len,
				  charset, encoding, charset_flags);
    }
}

static int search_text_match(message_t *m, const union search_value *v,
			     void *internalised, void *data1)
{
    struct searchmsg_rock sr;

    sr.substr = v->s;
    sr.pat = (comp_pat *)internalised;
    sr.skipheader = (int)(unsigned long)data1;
    return message_foreach_text_section(m, searchmsg_cb, &sr);
}

/* ====================================================================== */

static hash_table attrs_by_name = HASH_TABLE_INITIALIZER;

/*
 * Call search_attr_init() before doing any work with search
 * expressions.
 */
EXPORTED void search_attr_init(void)
{
    unsigned int i;

    static const search_attr_t attrs[] = {
	{
	    "bcc",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_bcc
	},{
	    "cc",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_cc
	},{
	    "from",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_from
	},{
	    "message-id",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_messageid
	},{
	    "listid",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_listid_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    NULL
	},{
	    "contenttype",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_contenttype_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    NULL
	},{
	    "subject",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_subject
	},{
	    "to",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_string_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)message_get_to
	},{
	    "msgno",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_seq_match,
	    search_seq_serialise,
	    search_seq_unserialise,
	    search_seq_duplicate,
	    search_seq_free,
	    (void *)message_get_msgno
	},{
	    "uid",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_seq_match,
	    search_seq_serialise,
	    search_seq_unserialise,
	    search_seq_duplicate,
	    search_seq_free,
	    (void *)message_get_uid
	},{
	    "systemflags",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_flags_match,
	    search_systemflags_serialise,
	    search_systemflags_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_systemflags
	},{
	    "indexflags",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_flags_match,
	    search_indexflags_serialise,
	    search_indexflags_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_indexflags
	},{
	    "keyword",
	    search_keyword_internalise,
	    /*cmp*/NULL,
	    search_keyword_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    NULL
	},{
	    "convflags",
	    search_convflags_internalise,
	    /*cmp*/NULL,
	    search_convflags_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    NULL
	},{
	    "convmodseq",
	    search_convmodseq_internalise,
	    /*cmp*/NULL,
	    search_convmodseq_match,
	    search_uint64_serialise,
	    search_uint64_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    NULL
	},{
	    "modseq",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_uint64_match,
	    search_uint64_serialise,
	    search_uint64_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_modseq
	},{
	    "cid",
	    /*internalise*/NULL,
	    /*cmp*/NULL,
	    search_uint64_match,
	    search_cid_serialise,
	    search_cid_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_cid
	},{
	    "folder",
	    search_folder_internalise,
	    /*cmp*/NULL,
	    search_folder_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)NULL
	},{
	    "annotation",
	    search_annotation_internalise,
	    /*cmp*/NULL,
	    search_annotation_match,
	    search_annotation_serialise,
	    search_annotation_unserialise,
	    search_annotation_duplicate,
	    search_annotation_free,
	    (void *)NULL
	},{
	    "size",
	    /*internalise*/NULL,
	    search_uint32_cmp,
	    search_uint32_match,
	    search_uint32_serialise,
	    search_uint32_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_size
	},{
	    "internaldate",
	    /*internalise*/NULL,
	    search_uint32_cmp,
	    search_uint32_match,
	    search_uint32_serialise,
	    search_uint32_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_internaldate
	},{
	    "sentdate",
	    /*internalise*/NULL,
	    search_uint32_cmp,
	    search_uint32_match,
	    search_uint32_serialise,
	    search_uint32_unserialise,
	    /*duplicate*/NULL,
	    /*free*/NULL,
	    (void *)message_get_sentdate
	},{
	    "body",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_text_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)1	    /* skipheader flag */
	},{
	    "text",
	    search_string_internalise,
	    /*cmp*/NULL,
	    search_text_match,
	    search_string_serialise,
	    search_string_unserialise,
	    search_string_duplicate,
	    search_string_free,
	    (void *)0	    /* skipheader flag */
	}
    };

    construct_hash_table(&attrs_by_name, VECTOR_SIZE(attrs), 0);
    for (i = 0 ; i < VECTOR_SIZE(attrs) ; i++)
	hash_insert(attrs[i].name, (void *)&attrs[i], &attrs_by_name);
}

/*
 * Find and return a search attribute by name.  Used when building
 * comparison nodes in a search expression tree.  Name comparison is
 * case insensitive.  Returns a pointer to static data or NULL if there
 * is no attribute of the given name.
 */
EXPORTED const search_attr_t *search_attr_find(const char *name)
{
    char tmp[128];

    strlcpy(tmp, name, sizeof(tmp));
    lcase(tmp);
    return hash_lookup(tmp, &attrs_by_name);
}

/*
 * Find and return a search attribute for the named header field.  Used
 * when building comparison nodes for the HEADER search criterion in a
 * search expression tree.  Field name comparison is case insensitive.
 * Returns a pointer to internally managed data or NULL if there is no
 * attribute of the given name.
 */
EXPORTED const search_attr_t *search_attr_find_field(const char *field)
{
    search_attr_t *attr;
    char *key = NULL;
    static const search_attr_t proto = {
	"name",
	search_string_internalise,
	/*cmp*/NULL,
	search_header_match,
	search_string_serialise,
	search_string_unserialise,
	search_string_duplicate,
	search_string_free,
	NULL
    };

    /* some header fields can be reduced to search terms */
    if (!strcasecmp(field, "bcc") ||
	!strcasecmp(field, "cc") ||
	!strcasecmp(field, "to") ||
	!strcasecmp(field, "from") ||
	!strcasecmp(field, "subject") ||
	!strcasecmp(field, "message-id"))
	return search_attr_find(field);

    key = lcase(strconcat("header:", field, (char *)NULL));
    attr = (search_attr_t *)hash_lookup(key, &attrs_by_name);

    if (!attr) {
	attr = (search_attr_t *)xzmalloc(sizeof(search_attr_t));
	*attr = proto;
	attr->name = key;
	attr->data1 = strchr(key, ':')+1;
	hash_insert(attr->name, (void *)attr, &attrs_by_name);
	key = NULL;	/* attr takes this over */
    }

    free(key);
    return attr;
}