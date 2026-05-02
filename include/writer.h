#ifndef MINIM_WRITER_H
#define MINIM_WRITER_H

#include "minim.h"

#include <stdio.h>

/* ----------------------------------------------------------------------
 * Writer
 *
 * Mwrite emits one datum to `out` in s-expression form. The output is
 * intended to be read back by Mread for the types that v1 supports.
 *
 * Mwrite does not allocate on the GC heap, so the input value does not
 * need to be protected across the call.
 * -------------------------------------------------------------------- */

void Mwrite(mobj v, FILE *out);

#endif /* MINIM_WRITER_H */
