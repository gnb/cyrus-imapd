/* bitvector.c -- bit vector functions
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
#include <string.h>

#include "xmalloc.h"
#include "bitvector.h"

#ifndef MAX
#define MAX(a,b)    ((a)>(b)?(a):(b))
#endif

#define vidx(x)		((x) >> 3)
#define vmask(x)	(1 << ((x) & 0x7))
#define vtailmask(x)	((unsigned char)(0xff << ((x) & 0x7)))
#define vlen(x)		vidx((x)+7)
#define QUANTUM		(256)

/* Ensure that the array contains enough memory for @len
 * bits, expanding the bitvector if necessary */
static void bv_ensure(bitvector_t *bv, unsigned int len)
{
    len = vlen(len);	    /* now number of bytes */
    if (len > bv->alloc) {
	unsigned int newalloc = ((len + QUANTUM-1) / QUANTUM) * QUANTUM;
	bv->bits = (unsigned char *)xrealloc(bv->bits, newalloc);
	memset(bv->bits + bv->alloc, 0, newalloc - bv->alloc);
	bv->alloc = newalloc;
    }
}

void bv_setsize(bitvector_t *bv, unsigned int len)
{
    bv_ensure(bv, len);
    if (len < bv->length) {
	/* shrinking - need to clear old bits */
	memset(bv->bits+vlen(len), 0, vlen(bv->length) - vlen(len));
	bv->bits[vidx(len)] &= ~vtailmask(len);
    }
    bv->length = len;
}

void bv_prealloc(bitvector_t *bv, unsigned int len)
{
    bv_ensure(bv, len);
}

void bv_clearall(bitvector_t *bv)
{
    if (bv->length)
	memset(bv->bits, 0, vlen(bv->length));
}

void bv_setall(bitvector_t *bv)
{
    if (bv->length)
	memset(bv->bits, 0xff, vlen(bv->length));
}

int bv_isset(const bitvector_t *bv, unsigned int i)
{
    if (i >= bv->length)
	return 0;
    return !!(bv->bits[vidx(i)] & vmask(i));
}

void bv_set(bitvector_t *bv, unsigned int i)
{
    bv_ensure(bv, i+1);
    bv->bits[vidx(i)] |= vmask(i);
    if (i >= bv->length)
	bv->length = i+1;
}

void bv_clear(bitvector_t *bv, unsigned int i)
{
    if (i < bv->length) {
	bv_ensure(bv, i+1);
	bv->bits[vidx(i)] &= ~vmask(i);
    }
}

void bv_andeq(bitvector_t *a, const bitvector_t *b)
{
    unsigned int n;
    unsigned int i;

    bv_ensure(a, b->length);
    if (!a->length)
	return;
    n = vlen(b->length+1);
    for (i = 0 ; i <= n ; i++)
	a->bits[i] &= b->bits[i];
    n = vlen(a->length);
    for ( ; i <= n ; i++)
	a->bits[i] = 0;
    a->length = MAX(a->length, b->length);
}

void bv_oreq(bitvector_t *a, const bitvector_t *b)
{
    unsigned int n;
    unsigned int i;

    bv_ensure(a, b->length);
    if (!a->length)
	return;
    n = vlen(b->length+1);
    for (i = 0 ; i <= n ; i++)
	a->bits[i] |= b->bits[i];
    a->length = MAX(a->length, b->length);
}

void bv_free(bitvector_t *bv)
{
    if (bv->alloc)
	free(bv->bits);
    bv->length = 0;
    bv->alloc = 0;
    bv->bits = NULL;
}

