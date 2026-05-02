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

#define HEAP_INITIAL_BYTES (1024 * 1024) /* 1 MiB per semispace */

#define MINIM_ALIGN_BYTES 16
#define MINIM_ALIGN(n)    (((size_t)(n) + (MINIM_ALIGN_BYTES - 1)) & ~(size_t)(MINIM_ALIGN_BYTES - 1))

#define MINIM_PAIR_SIZE    16
#define MINIM_FLONUM_SIZE  16
#define MINIM_SYMBOL_SIZE  16
#define MINIM_CLOSURE_SIZE 32  /* 4 slots × 8 bytes, no header */

/* Vector length is packed into the upper 60 bits of the type word
 * (`(length << 4) | MSEC_VECTOR`), so anything past 2^60 - 1 would lose
 * bits on the way in. The byte-size formula `8 + 8 * length` also stays
 * well clear of size_t overflow at this bound. */
#define MINIM_VECTOR_MAX_LENGTH (((size_t)1 << 60) - 1)

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
