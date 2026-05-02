#ifndef MINIM_GC_H
#define MINIM_GC_H

#include <stddef.h>
#include "minim.h"

/* ----------------------------------------------------------------------
 * Heap layout details
 *
 * Internal: used by alloc.c, gc.c, and symbol.c when walking raw heap
 * bytes. The publicly visible accessors live in minim.h.
 * -------------------------------------------------------------------- */

#define MINIM_ALIGN_BYTES 16
#define MINIM_ALIGN(n)    (((size_t)(n) + (MINIM_ALIGN_BYTES - 1)) & ~(size_t)(MINIM_ALIGN_BYTES - 1))

#define MINIM_PAIR_SIZE   16
#define MINIM_FLONUM_SIZE 16
#define MINIM_SYMBOL_SIZE 16

static inline size_t minim_vector_size(size_t length) {
    return MINIM_ALIGN(8 + 8 * length);
}

/* ----------------------------------------------------------------------
 * GC entry points
 * -------------------------------------------------------------------- */

void gc_init(size_t initial_bytes);
void gc_shutdown(void);
char *gc_alloc(size_t n);
void gc_collect(size_t need);

#endif /* MINIM_GC_H */
