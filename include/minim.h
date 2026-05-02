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
#define Mfalse     ((mobj)0x06)
#define Mtrue      ((mobj)0x0E)
#define Mnull      ((mobj)0x16)

/* ----------------------------------------------------------------------
 * Predicates
 * -------------------------------------------------------------------- */

static inline int Mfixnump(mobj v)    { return (v & MTAG_MASK) == MTAG_FIXNUM; }
static inline int Mpairp(mobj v)      { return (v & MTAG_MASK) == MTAG_PAIR; }
static inline int Mflonump(mobj v)    { return (v & MTAG_MASK) == MTAG_FLONUM; }
static inline int Msymbolp(mobj v)    { return (v & MTAG_MASK) == MTAG_SYMBOL; }
static inline int Mimmediatep(mobj v) { return (v & MTAG_MASK) == MTAG_IMMEDIATE; }
static inline int Mtyped_objp(mobj v) { return (v & MTAG_MASK) == MTAG_TYPED_OBJ; }

static inline int Mnullp(mobj v)  { return v == Mnull; }
static inline int Mtruep(mobj v)  { return v == Mtrue; }
static inline int Mfalsep(mobj v) { return v == Mfalse; }
static inline int Mbooleanp(mobj v) { return (v & ~(mobj)0x08) == Mfalse; }

static inline int Mvectorp(mobj v) {
    if (!Mtyped_objp(v)) return 0;
    mobj header = *(mobj *)((uintptr_t)v - MTAG_TYPED_OBJ);
    return (header & MSEC_MASK) == MSEC_VECTOR;
}

/* ----------------------------------------------------------------------
 * Constructors and accessors for immediates / fixnums
 * -------------------------------------------------------------------- */

static inline mobj  Mfixnum(intptr_t n) { return (mobj)((uintptr_t)n << 3); }
static inline intptr_t Mfixnum_val(mobj v) { return (intptr_t)v >> 3; }

/* ----------------------------------------------------------------------
 * Pair accessors / mutators
 * -------------------------------------------------------------------- */

static inline mobj *Mpair_slots(mobj v) {
    return (mobj *)((uintptr_t)v - MTAG_PAIR);
}

static inline mobj Mcar(mobj v) { return Mpair_slots(v)[0]; }
static inline mobj Mcdr(mobj v) { return Mpair_slots(v)[1]; }

static inline void Mset_car(mobj p, mobj v) { Mpair_slots(p)[0] = v; }
static inline void Mset_cdr(mobj p, mobj v) { Mpair_slots(p)[1] = v; }

/* ----------------------------------------------------------------------
 * Flonum accessor
 * -------------------------------------------------------------------- */

static inline double Mflonum_val(mobj v) {
    double d;
    /* offset 8: skip the 8-byte header */
    __builtin_memcpy(&d, (void *)((uintptr_t)v - MTAG_FLONUM + 8), sizeof(d));
    return d;
}

/* ----------------------------------------------------------------------
 * Vector accessors / mutators
 * -------------------------------------------------------------------- */

static inline mobj *Mvector_header(mobj v) {
    return (mobj *)((uintptr_t)v - MTAG_TYPED_OBJ);
}

static inline size_t Mvector_length(mobj v) {
    return (size_t)(Mvector_header(v)[0] >> 4);
}

static inline mobj *Mvector_slots(mobj v) {
    return Mvector_header(v) + 1;
}

static inline mobj Mvector_ref(mobj v, size_t i) {
    return Mvector_slots(v)[i];
}

static inline void Mvector_set(mobj v, size_t i, mobj x) {
    Mvector_slots(v)[i] = x;
}

/* ----------------------------------------------------------------------
 * Symbol accessor
 * -------------------------------------------------------------------- */

static inline const char *Msymbol_name(mobj v) {
    return *(const char **)((uintptr_t)v - MTAG_SYMBOL + 8);
}

/* ----------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------- */

void Minit(void);
void Mshutdown(void);

mobj Mcons(mobj car, mobj cdr);
mobj Mvector(size_t length, mobj fill);
mobj Mflonum(double d);
mobj Mintern(const char *name);

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
