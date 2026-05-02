#ifndef MINIM_H
#define MINIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
/* 0x4 reserved for future use (e.g. strings) */
#define MTAG_CLOSURE    ((mobj)0x5)
#define MTAG_IMMEDIATE  ((mobj)0x6)
#define MTAG_TYPED_OBJ  ((mobj)0x7)

#define MTAG_MASK       ((mobj)0x7)

/* Secondary tags for typed objects (low 4 bits of the type word).
 *
 * Every typed object shares the same on-heap shape: a header word
 *   (slot_count << 4) | secondary_tag
 * followed by `slot_count` payload slots, each one machine word wide
 * (mobj or a fixnum-shifted raw word). The GC traces every payload
 * slot uniformly; only the secondary tag's *interpretation* differs
 * between kinds. See docs/EVAL.md for the per-kind slot layout.
 *
 * Closures get their own primary tag (MTAG_CLOSURE) rather than a
 * secondary tag here — calling a procedure is the hottest path in a
 * Scheme runtime, so the predicate cost should be one instruction. */
#define MSEC_VECTOR     ((mobj)0x0)
#define MSEC_KONT       ((mobj)0x1)  /* spaghetti-stack frame */
#define MSEC_ENV        ((mobj)0x3)  /* lexical environment frame */
#define MSEC_PRIM       ((mobj)0x4)  /* built-in procedure */
#define MSEC_CONT       ((mobj)0x5)  /* first-class captured continuation */
#define MSEC_MASK       ((mobj)0xF)

/* Continuation frame kinds, stored as a fixnum in slot 0 of an
 * MSEC_KONT typed object. The constants are pre-shifted (Mfixnum(N))
 * so they read as MTAG_FIXNUM (a leaf) to the GC. See docs/EVAL.md
 * for the per-kind extra slots. KONT_EXC is reserved for the future
 * exception-handler frame and is not pushed in v1. */
#define KONT_HALT       ((mobj)(0 << 3))
#define KONT_IF         ((mobj)(1 << 3))
#define KONT_SEQ        ((mobj)(2 << 3))
#define KONT_APP        ((mobj)(3 << 3))
#define KONT_SET        ((mobj)(4 << 3))
#define KONT_DEFINE     ((mobj)(5 << 3))
#define KONT_EXC        ((mobj)(6 << 3))

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

static inline bool Mhas_secondary(mobj v, mobj sec) {
    if ((v & MTAG_MASK) != MTAG_TYPED_OBJ) return false;
    mobj header = *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ));
    return (header & MSEC_MASK) == sec;
}

static inline bool Mclosurep(mobj v) { return (v & MTAG_MASK) == MTAG_CLOSURE; }
static inline bool Menvp(mobj v)     { return Mhas_secondary(v, MSEC_ENV); }
static inline bool Mkontp(mobj v)    { return Mhas_secondary(v, MSEC_KONT); }
static inline bool Mprimp(mobj v)    { return Mhas_secondary(v, MSEC_PRIM); }
static inline bool Mcontp(mobj v)    { return Mhas_secondary(v, MSEC_CONT); }
static inline bool Mprocedurep(mobj v) {
    return Mclosurep(v) || Mprimp(v) || Mcontp(v);
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

/* Generic typed-object slot count and slot access. Works for every
 * typed-object kind (vector, kont, closure, env, prim, cont) because
 * they all share the `(slot_count << 4) | sec` header layout. */
static inline size_t Mtyped_obj_slots(mobj v) {
    return (size_t)(*((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ)) >> 4);
}
static inline mobj Mtyped_obj_ref(mobj v, size_t i) {
    return *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ) + 1 + i);
}

/* Closure: own primary tag, four fixed slots, no header word. */
static inline mobj *Mclosure_slots(mobj v) {
    return (mobj *)((uintptr_t)v - MTAG_CLOSURE);
}
static inline mobj Mclosure_params(mobj v) { return Mclosure_slots(v)[0]; }
static inline mobj Mclosure_body(mobj v)   { return Mclosure_slots(v)[1]; }
static inline mobj Mclosure_env(mobj v)    { return Mclosure_slots(v)[2]; }
static inline mobj Mclosure_name(mobj v)   { return Mclosure_slots(v)[3]; }

/* Per-kind accessors for the remaining typed-object kinds. Each just
 * wraps Mtyped_obj_ref with a name that documents the slot's role;
 * see docs/EVAL.md for layouts. */

static inline mobj Menv_rib(mobj v)        { return Mtyped_obj_ref(v, 0); }
static inline mobj Menv_parent(mobj v)     { return Mtyped_obj_ref(v, 1); }

static inline mobj Mkont_kind(mobj v)      { return Mtyped_obj_ref(v, 0); }
static inline mobj Mkont_parent(mobj v)    { return Mtyped_obj_ref(v, 1); }
static inline mobj Mkont_env(mobj v)       { return Mtyped_obj_ref(v, 2); }

static inline mobj Mprim_name(mobj v)      { return Mtyped_obj_ref(v, 0); }
static inline mobj Mprim_arity_min(mobj v) { return Mtyped_obj_ref(v, 1); }
static inline mobj Mprim_arity_max(mobj v) { return Mtyped_obj_ref(v, 2); }

static inline mobj Mcont_kont(mobj v)      { return Mtyped_obj_ref(v, 0); }

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
static inline void Mtyped_obj_set(mobj v, size_t i, mobj x) {
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

/* Evaluator object constructors. See docs/EVAL.md. */
mobj Mclosure(mobj params, mobj body, mobj env, mobj name);
mobj Menv_extend(mobj rib, mobj parent);

/* Mkont allocates a frame with `extra_slots` slots beyond the
 * standard (kind, parent, env). The caller fills slots 3..3+extra-1
 * via Mtyped_obj_set (or via a per-kind constructor below). */
mobj Mkont(mobj kind, mobj parent, mobj env, size_t extra_slots);
mobj Mkont_if(mobj parent, mobj env, mobj then_expr, mobj else_expr);
mobj Mkont_seq(mobj parent, mobj env, mobj rest);
mobj Mkont_app(mobj parent, mobj env, mobj unev, mobj evald);
mobj Mkont_set(mobj parent, mobj env, mobj name);
mobj Mkont_define(mobj parent, mobj env, mobj name);

/* Function pointer for a primitive procedure. Receives the
 * already-evaluated argument list as a Scheme list and returns one
 * mobj. The eval loop is in APPLY mode after this returns. */
typedef mobj (*Mprim_fn)(mobj args);
mobj Mprim(const char *name, Mprim_fn fn, intptr_t arity_min, intptr_t arity_max);
Mprim_fn Mprim_fn_of(mobj v);

mobj Mcont(mobj kont);

/* ----------------------------------------------------------------------
 * System
 * -------------------------------------------------------------------- */

void Minit(void);
void Mshutdown(void);

/* ----------------------------------------------------------------------
 * Reader / writer
 *
 * `mreader` wraps either a C string or a FILE *; `Mread` consumes one
 * datum and returns it (or `Meof` at end of input). Allocations during
 * reading are subject to the GC, so callers holding the result across
 * further reads must protect it.
 *
 * `Mwrite` emits one datum to `out` in s-expression form. It does not
 * allocate on the GC heap, so the input value does not need protection
 * across the call.
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
void Mwrite(mobj v, FILE *out);

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
