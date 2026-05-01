#ifndef MINIM_H
#define MINIM_H

#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------------
 * Version
 * -------------------------------------------------------------------- */

#define MINIM_MAJOR_VERSION 0
#define MINIM_MINOR_VERSION 4
#define MINIM_PATCH_VERSION 0
#define MINIM_VERSION_STRING "0.4.0"

/* ----------------------------------------------------------------------
 * Value type
 *
 * Every Scheme value is one machine word. The bottom 3 bits hold a
 * primary type tag; heap objects are 16-byte aligned, so 4 low bits of
 * any heap pointer are always zero (we use 3 of them for the tag).
 * -------------------------------------------------------------------- */

typedef uintptr_t mobj;

/* Primary tags */
#define MTAG_FIXNUM     ((mobj)0x0)
#define MTAG_PAIR       ((mobj)0x1)
#define MTAG_FLONUM     ((mobj)0x2)
#define MTAG_SYMBOL     ((mobj)0x3)
/* 0x4 and 0x5 reserved for future use (closures, strings) */
#define MTAG_IMMEDIATE  ((mobj)0x6)
#define MTAG_TYPED_OBJ  ((mobj)0x7)

#define MTAG_MASK       ((mobj)0x7)

/* Secondary tags for typed objects (low 4 bits of the type word) */
#define MSEC_VECTOR     ((mobj)0x0)
#define MSEC_MASK       ((mobj)0xF)

/* Forwarding marker written into the first word of a copied object during GC */
#define MFORWARD_MARKER ((mobj)0x3E)

/* Immediate values */
#define MINIM_FALSE     ((mobj)0x06)
#define MINIM_TRUE      ((mobj)0x0E)
#define MINIM_NULL      ((mobj)0x16)

/* ----------------------------------------------------------------------
 * Predicates
 * -------------------------------------------------------------------- */

static inline int minim_fixnump(mobj v)    { return (v & MTAG_MASK) == MTAG_FIXNUM; }
static inline int minim_pairp(mobj v)      { return (v & MTAG_MASK) == MTAG_PAIR; }
static inline int minim_flonump(mobj v)    { return (v & MTAG_MASK) == MTAG_FLONUM; }
static inline int minim_symbolp(mobj v)    { return (v & MTAG_MASK) == MTAG_SYMBOL; }
static inline int minim_immediatep(mobj v) { return (v & MTAG_MASK) == MTAG_IMMEDIATE; }
static inline int minim_typed_objp(mobj v) { return (v & MTAG_MASK) == MTAG_TYPED_OBJ; }

static inline int minim_nullp(mobj v)  { return v == MINIM_NULL; }
static inline int minim_truep(mobj v)  { return v == MINIM_TRUE; }
static inline int minim_falsep(mobj v) { return v == MINIM_FALSE; }
static inline int minim_booleanp(mobj v) { return (v & ~(mobj)0x08) == MINIM_FALSE; }

static inline int minim_vectorp(mobj v) {
    if (!minim_typed_objp(v)) return 0;
    mobj header = *(mobj *)((uintptr_t)v - MTAG_TYPED_OBJ);
    return (header & MSEC_MASK) == MSEC_VECTOR;
}

/* ----------------------------------------------------------------------
 * Constructors and accessors for immediates / fixnums
 * -------------------------------------------------------------------- */

static inline mobj  minim_make_fixnum(intptr_t n) { return (mobj)((uintptr_t)n << 3); }
static inline intptr_t minim_fixnum_value(mobj v) { return (intptr_t)v >> 3; }

/* ----------------------------------------------------------------------
 * Pair accessors / mutators
 * -------------------------------------------------------------------- */

static inline mobj *minim_pair_slots(mobj v) {
    return (mobj *)((uintptr_t)v - MTAG_PAIR);
}

static inline mobj minim_car(mobj v) { return minim_pair_slots(v)[0]; }
static inline mobj minim_cdr(mobj v) { return minim_pair_slots(v)[1]; }

static inline void minim_set_car(mobj p, mobj v) { minim_pair_slots(p)[0] = v; }
static inline void minim_set_cdr(mobj p, mobj v) { minim_pair_slots(p)[1] = v; }

/* ----------------------------------------------------------------------
 * Flonum accessor
 * -------------------------------------------------------------------- */

static inline double minim_flonum_value(mobj v) {
    double d;
    /* offset 8: skip the 8-byte header */
    __builtin_memcpy(&d, (void *)((uintptr_t)v - MTAG_FLONUM + 8), sizeof(d));
    return d;
}

/* ----------------------------------------------------------------------
 * Vector accessors / mutators
 * -------------------------------------------------------------------- */

static inline mobj *minim_vector_header(mobj v) {
    return (mobj *)((uintptr_t)v - MTAG_TYPED_OBJ);
}

static inline size_t minim_vector_length(mobj v) {
    return (size_t)(minim_vector_header(v)[0] >> 4);
}

static inline mobj *minim_vector_slots(mobj v) {
    return minim_vector_header(v) + 1;
}

static inline mobj minim_vector_ref(mobj v, size_t i) {
    return minim_vector_slots(v)[i];
}

static inline void minim_vector_set(mobj v, size_t i, mobj x) {
    minim_vector_slots(v)[i] = x;
}

/* ----------------------------------------------------------------------
 * Symbol accessor
 * -------------------------------------------------------------------- */

static inline const char *minim_symbol_name(mobj v) {
    return *(const char **)((uintptr_t)v - MTAG_SYMBOL + 8);
}

/* ----------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------- */

void minim_init(void);
void minim_shutdown(void);

mobj minim_cons(mobj car, mobj cdr);
mobj minim_make_vector(size_t length, mobj fill);
mobj minim_make_flonum(double d);
mobj minim_intern(const char *name);

/* ----------------------------------------------------------------------
 * Roots: shadow stack + globals
 *
 * Use MINIM_GC_FRAME_BEGIN/END to bracket any C function that holds
 * Scheme values across an allocation. Protect each such local with
 * MINIM_GC_PROTECT(v); the macro pushes &v onto the shadow stack so
 * the collector can rewrite it after a copy.
 * -------------------------------------------------------------------- */

extern mobj **minim_shadow_stack;
extern size_t minim_ssp;
extern size_t minim_ssp_capacity;

void minim_shadow_stack_grow(void);

#define MINIM_GC_FRAME_BEGIN \
    size_t __minim_save_ssp = minim_ssp

#define MINIM_GC_PROTECT(v)                                  \
    do {                                                     \
        if (minim_ssp >= minim_ssp_capacity)                 \
            minim_shadow_stack_grow();                       \
        minim_shadow_stack[minim_ssp++] = (mobj *)&(v);      \
    } while (0)

#define MINIM_GC_PROTECT2(a, b)        \
    do { MINIM_GC_PROTECT(a); MINIM_GC_PROTECT(b); } while (0)

#define MINIM_GC_PROTECT3(a, b, c)     \
    do { MINIM_GC_PROTECT(a); MINIM_GC_PROTECT(b); MINIM_GC_PROTECT(c); } while (0)

#define MINIM_GC_FRAME_END \
    (minim_ssp = __minim_save_ssp)

#define MINIM_GC_RETURN(x)                              \
    do { mobj __r = (x); MINIM_GC_FRAME_END; return __r; } while (0)

void minim_protect(mobj *slot);

#endif /* MINIM_H */
