#ifndef MINIM_H
#define MINIM_H

#include <stdbool.h>
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
 * Types
 * -------------------------------------------------------------------- */

typedef uintptr_t mobj;
typedef intptr_t mint;
typedef unsigned int mchar;
typedef unsigned char mbyte;

/* ----------------------------------------------------------------------
 * Type tags
 *
 * Every Scheme value is one machine word. The bottom 3 bits hold a
 * primary type tag; heap objects are 16-byte aligned, so 4 low bits of
 * any heap pointer are always zero (we use 3 of them for the tag).
 * -------------------------------------------------------------------- */

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

/* ----------------------------------------------------------------------
 * Immediate values
 * -------------------------------------------------------------------- */

#define Mtrue      ((mobj)0x0E)
#define Mfalse     ((mobj)0x06)
#define Mnull      ((mobj)0x26)
#define Meof       ((mobj)0x36)

/* ----------------------------------------------------------------------
 * Predicates
 * -------------------------------------------------------------------- */

static inline bool Mnullp(mobj v)  {
    return v == Mnull;
}
static inline bool Mtruep(mobj v)  {
    return v == Mtrue;
}
static inline bool Mfalsep(mobj v) {
    return v == Mfalse;
}
static inline bool Meofp(mobj v)   {
    return v == Meof;
}
static inline bool Mbooleanp(mobj v) {
    return (v & 0xF7) == 0x06; // matches both #t and #f
}
static inline bool Mfixnump(mobj v) {
    return (v & MTAG_MASK) == MTAG_FIXNUM;
}
static inline bool Mpairp(mobj v) {
    return (v & MTAG_MASK) == MTAG_PAIR;
}
static inline bool Mflonump(mobj v) {
    return (v & MTAG_MASK) == MTAG_FLONUM;
}
static inline bool Msymbolp(mobj v) {
    return (v & MTAG_MASK) == MTAG_SYMBOL;
}
static inline bool Mvectorp(mobj v) {
    if ((v & MTAG_MASK) != MTAG_TYPED_OBJ) {
        return false;
    }

    mobj header = *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ));
    return (header & MSEC_MASK) == MSEC_VECTOR;
}

/* ----------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------- */

static inline bool Mboolean_val(mobj v) {
    return v == Mtrue;
}
static inline mint Mfixnum_val(mobj v) {
    return (mint)v >> 3;
}
static inline mobj Mcar(mobj v) {
    return *((mobj *)((uintptr_t)v - MTAG_PAIR));
}
static inline mobj Mcdr(mobj v) {
    return *((mobj *)((uintptr_t)v - MTAG_PAIR) + 1);
}
static inline double Mflonum_val(mobj v) {
    return *((double *)(((uintptr_t)v - MTAG_FLONUM) + 8));
}
static inline size_t Mvector_length(mobj v) {
    return (size_t)(*((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ)) >> 4);
}
static inline mobj Mvector_ref(mobj v, size_t i) {
    return *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ) + 1 + i);
}

/* ----------------------------------------------------------------------
 * Mutators
 * -------------------------------------------------------------------- */

static inline void Mset_car(mobj p, mobj v) {
    *((mobj *)((uintptr_t)p - MTAG_PAIR)) = v;
}
static inline void Mset_cdr(mobj p, mobj v) {
    *((mobj *)((uintptr_t)p - MTAG_PAIR) + 1) = v;
}
static inline void Mvector_set(mobj v, size_t i, mobj x) {
    *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ) + 1 + i) = x;
}
static inline const char *Msymbol_name(mobj v) {
    return *(const char **)((uintptr_t)v - MTAG_SYMBOL + 8);
}

/* ----------------------------------------------------------------------
 * Constructors
 * -------------------------------------------------------------------- */

static inline mobj Mboolean(bool b) {
    return b ? Mtrue : Mfalse;
}
static inline mobj Mfixnum(intptr_t n) {
    return (mobj)((uintptr_t)n << 3);
}

mobj Mcons(mobj car, mobj cdr);
mobj Mvector(size_t length, mobj fill);
mobj Mflonum(double d);
mobj Mintern(const char *name);

/* ----------------------------------------------------------------------
 * System
 * -------------------------------------------------------------------- */

void Minit(void);
void Mshutdown(void);

/* ----------------------------------------------------------------------
 * Interned symbols
 * -------------------------------------------------------------------- */

 extern mobj quote_sym;

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

void minim_protect(mobj *slot);
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

#endif /* MINIM_H */
