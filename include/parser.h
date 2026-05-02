#ifndef MINIM_PARSER_H
#define MINIM_PARSER_H

#include "minim.h"

#include <stddef.h>
#include <stdio.h>

/* ----------------------------------------------------------------------
 * Reader / s-expression parser
 *
 * One reader state object can be backed by either a C string or a
 * FILE *. `Mread` consumes one datum and returns it (or `Meof` at end
 * of input).
 *
 * Allocations performed by the parser are subject to the GC; callers
 * holding the result across further reads must protect it like any
 * other mobj.
 * -------------------------------------------------------------------- */

typedef enum {
    MREADER_STRING,
    MREADER_FILE,
} mreader_kind;

typedef struct mreader {
    mreader_kind kind;
    union {
        struct {
            const char *buf;
            size_t pos, len;
        } s;
        FILE *fp;
    } u;
    int peeked; /* -1 if no peeked char buffered */
} mreader;

void mreader_init_string(mreader *r, const char *s);
void mreader_init_file(mreader *r, FILE *fp);

mobj Mread(mreader *r);

#endif /* MINIM_PARSER_H */
