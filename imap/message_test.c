/*
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

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <sys/stat.h>

/* cyrus includes */
#include "assert.h"
#include "bsearch.h"
#include "exitcodes.h"
#include "global.h"
#include "imap_err.h"
#include "index.h"
#include "conversations.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "message.h"
#include "sysexits.h"
#include "util.h"
#include "xmalloc.h"

#if !HAVE___ATTRIBUTE__
#define __attribute__(x)
#endif

/* config.c stuff */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;
static int usage(const char *name);

int verbose = 0;
enum { PART_TREE, TEXT_SECTIONS } dump_mode = PART_TREE;

static const char *indent(int depth)
{
    static struct buf buf = BUF_INITIALIZER;
    static int last_depth = -1;
    int i;

    if (depth != last_depth) {
	buf_reset(&buf);
	for (i = 0 ; i < depth ; i++)
	    buf_appendcstr(&buf, "  ");
	last_depth = depth;
    }
    return buf_cstring(&buf);
}

static int dump_part(part_t *part, unsigned int id, int depth)
{
    int r;
    int i;
    const char *s;
    unsigned int numparts;
    part_t *child;

    if (depth) {
	printf("%sPART %u\n", indent(depth), id);

	r = part_get_type(part, &s);
	if (r < 0) return r;
	printf("%sTYPE:%s\n", indent(depth+1), s);

	r = part_get_subtype(part, &s);
	if (r < 0) return r;
	printf("%sSUBTYPE:%s\n", indent(depth+1), s);

	r = part_get_encoding(part, &i);
	if (r < 0) return r;
	printf("%sENCODING:%s\n", indent(depth+1), encoding_name(i));

	r = part_get_charset(part, &i);
	if (r < 0) return r;
	printf("%sCHARSET:%s\n", indent(depth+1), charset_name(i));
    }

    r = part_get_num_parts(part, &numparts);
    if (r < 0) return r;
    if (depth)
	printf("%sNUMPARTS:%d\n", indent(depth+1), numparts);

    for (id = 1 ; id <= numparts ; id++) {
	r = part_get_part(part, id, &child);
	if (r) return r;
	r = dump_part(child, id, depth+1);
	if (r) return r;
    }

    return 0;
}


static int dump_part_tree(message_t *message)
{
    struct buf buf = BUF_INITIALIZER;
    part_t *root = NULL;
    const char *s;
    int i;
    unsigned int numparts;
    int r;

    printf("========================================\n");

    r = message_get_messageid(message, &buf);
    if (r < 0) return r;
    printf("MESSAGE-ID:%s\n", buf_cstring(&buf));
    buf_free(&buf);

    r = message_get_subject(message, &buf);
    if (r < 0) return r;
    printf("SUBJECT:%s\n", buf_cstring(&buf));
    buf_free(&buf);

    r = message_get_type(message, &s);
    if (r < 0) return r;
    printf("TYPE:%s\n", s);

    r = message_get_subtype(message, &s);
    if (r < 0) return r;
    printf("SUBTYPE:%s\n", s);

    r = message_get_encoding(message, &i);
    if (r < 0) return r;
    printf("ENCODING:%s\n", encoding_name(i));

    r = message_get_charset(message, &i);
    if (r < 0) return r;
    printf("CHARSET:%s\n", charset_name(i));

    r = message_get_num_parts(message, &numparts);
    if (r < 0) return r;
    printf("NUMPARTS:%d\n", numparts);

    r = message_get_root_part(message, &root);
    if (r < 0) return r;
    dump_part(root, 1, 0);

    return 0;
}

static int dump_one_section(int partno, int charset, int encoding,
			    struct buf *data,
			    void *rock __attribute__((unused)))
{
    fprintf(stderr, "SECTION %d charset=%s encoding=%s\n",
	    partno, charset_name(charset), encoding_name(encoding));
    fprintf(stderr, "-->");
    fwrite(data->s, 1, data->len, stderr);
    fprintf(stderr, "<--\n");
    return 0;
}

static int dump_text_sections(message_t *message)
{
    return message_foreach_text_section(message, dump_one_section, NULL);
}

static int dump_message(message_t *message)
{
    switch (dump_mode) {
    case PART_TREE: return dump_part_tree(message);
    case TEXT_SECTIONS: return dump_text_sections(message);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int c;
    const char *alt_config = NULL;
    const char *filename = NULL;
    const char *mboxname = NULL;
    int recno = 1;
    int record_flag = 0;
    int r = 0;

    if ((geteuid()) == 0 && (become_cyrus() != 0)) {
	fatal("must run as the Cyrus user", EC_USAGE);
    }

    while ((c = getopt(argc, argv, "Rf:m:pr:svC:")) != EOF) {
	switch (c) {

	case 'f':
	    filename = optarg;
	    break;

	case 'm':
	    mboxname = optarg;
	    break;

	case 'p':
	    dump_mode = PART_TREE;
	    break;

	case 'r':
	    recno = atoi(optarg);
	    if (recno <= 0)
		usage(argv[0]);
	    break;

	case 's':
	    dump_mode = TEXT_SECTIONS;
	    break;

	case 'v':
	    verbose++;
	    break;

	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	case 'R':
	    record_flag = 1;
	    break;

	default:
	    usage(argv[0]);
	    break;
	}
    }

    if (optind != argc)
	usage(argv[0]);
    if (mboxname && filename)
	usage(argv[0]);

    cyrus_init(alt_config, "message_test", 0);

    mboxlist_init(0);
    mboxlist_open(NULL);

    if (mboxname && record_flag) {
	struct mailbox *mailbox = NULL;
	struct index_record record;
	message_t *message = NULL;

	r = mailbox_open_irl(mboxname, &mailbox);
	if (r) {
	    fprintf(stderr, "Failed to open mailbox %s: %s\n",
		    mboxname, error_message(r));
	    return 1;
	}

	r = mailbox_read_index_record(mailbox, recno, &record);
	if (r) {
	    fprintf(stderr, "Failed to read index record %u of %s: %s\n",
		    recno, mboxname, error_message(r));
	    return 1;
	}

	message = message_new_from_record(mailbox, &record);
	r = dump_message(message);
	if (r) {
	    fprintf(stderr, "Error dumping message: %s\n",
		    error_message(r));
	    return 1;
	}

	message_unref(&message);
	mailbox_close(&mailbox);
    }
    else if (mboxname) {
	struct mailbox *mailbox = NULL;
	message_t *message = NULL;

	r = mailbox_open_irl(mboxname, &mailbox);
	if (r) {
	    fprintf(stderr, "Failed to open mailbox %s: %s\n",
		    mboxname, error_message(r));
	    return 1;
	}

	message = message_new_from_mailbox(mailbox, recno);
	r = dump_message(message);
	if (r) {
	    fprintf(stderr, "Error dumping message: %s\n",
		    error_message(r));
	    return 1;
	}

	message_unref(&message);
	mailbox_close(&mailbox);
    }
    else if (filename) {
	message_t *message = NULL;

	message = message_new_from_filename(filename);
	r = dump_message(message);
	if (r) {
	    fprintf(stderr, "Error dumping message: %s\n",
		    error_message(r));
	    return 1;
	}

	message_unref(&message);
    }
    else {
	message_t *message = NULL;
	int c;
	struct buf buf = BUF_INITIALIZER;

	while ((c = fgetc(stdin)) != EOF)
	    buf_putc(&buf, c);
	message = message_new_from_data(buf.s, buf.len);
	dump_message(message);
	if (r) {
	    fprintf(stderr, "Error dumping message: %s\n",
		    error_message(r));
	    return 1;
	}

	message_unref(&message);
	buf_free(&buf);
    }

    mboxlist_close();
    mboxlist_done();

    cyrus_done();

    return r;
}

static int usage(const char *name)
{
    fprintf(stderr, "usage: %s [format-options] -m mailbox [-r recno] [-R]\n", name);
    fprintf(stderr, "       %s [format-options] -f filename\n", name);
    fprintf(stderr, "       %s [format-options] < message\n", name);
    fprintf(stderr, "format-options :=\n");
    fprintf(stderr, "-p		dump message part tree\n");
    fprintf(stderr, "-s		dump text sections\n");
    exit(EC_USAGE);
}

void fatal(const char* s, int code)
{
    fprintf(stderr, "message_test: %s\n", s);
    cyrus_done();
    exit(code);
}

