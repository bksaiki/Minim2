#ifndef MINIM_TYPES_H
#define MINIM_TYPES_H

#include "minim.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Internal: object layout details, used by alloc.c, gc.c, and symbol.c.
 * The publicly visible accessors live in minim.h. This header is only
 * for code that walks raw heap bytes.
 * -------------------------------------------------------------------- */

#define MINIM_ALIGN_BYTES 16
#define MINIM_ALIGN(n)    (((size_t)(n) + (MINIM_ALIGN_BYTES - 1)) & ~(size_t)(MINIM_ALIGN_BYTES - 1))

#define MINIM_PAIR_BYTES   16
#define MINIM_FLONUM_BYTES 16
#define MINIM_SYMBOL_BYTES 16

static inline size_t minim_vector_bytes(size_t length) {
    return MINIM_ALIGN(8 + 8 * length);
}

/* Raw base pointer of a tagged value (untagged), valid only for heap tags */
static inline char *minim_untag(mobj v) {
    return (char *)((uintptr_t)v & ~(uintptr_t)MTAG_MASK);
}

/* Recover the original primary tag from a tagged value */
static inline mobj minim_tag_of(mobj v) {
    return v & MTAG_MASK;
}

/* True for "leaf" values that the GC never traces: fixnums and immediates.
 * A fixnum has tag 0; an immediate has tag 6 (MTAG_IMMEDIATE). */
static inline int minim_is_leaf(mobj v) {
    mobj tag = v & MTAG_MASK;
    return tag == MTAG_FIXNUM || tag == MTAG_IMMEDIATE;
}

#endif /* MINIM_TYPES_H */
