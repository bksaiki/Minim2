#include "minim.h"
#include "internal.h"

/* ======================================================================
 * Built-in primitive procedures.
 *
 * These are deliberately minimal — no argument-type checking. Passing
 * the wrong type is undefined behavior at this layer; e.g. (car 5)
 * reads garbage off the integer's would-be pair slot. Arity is checked
 * at the call site (in step_apply's KONT_APP arm) using the min/max
 * stored on the MSEC_PRIM object; type-checking and contract layers
 * belong above this one.
 *
 * GC discipline: each prim receives `args` as a Scheme list. The
 * caller protects `args` for the duration of the call, so prims may
 * allocate freely. A prim that reads from `args` *after* its own
 * allocation must protect `args` itself, since the bare parameter
 * slot would otherwise go stale.
 *
 * Registration happens in prims_register_all(), called from
 * eval_init() once the top-level env is in place.
 * ====================================================================== */

/* ----------------------------------------------------------------------
 * Type predicates
 * -------------------------------------------------------------------- */

static mobj prim_pair_p(mobj args)       { return Mboolean(Mpairp(Mcar(args))); }
static mobj prim_null_p(mobj args)       { return Mboolean(Mnullp(Mcar(args))); }
static mobj prim_symbol_p(mobj args)     { return Mboolean(Msymbolp(Mcar(args))); }
static mobj prim_boolean_p(mobj args)    { return Mboolean(Mbooleanp(Mcar(args))); }
static mobj prim_number_p(mobj args) {
    mobj v = Mcar(args);
    return Mboolean(Mfixnump(v) || Mflonump(v));
}
static mobj prim_procedure_p(mobj args)  { return Mboolean(Mprocedurep(Mcar(args))); }
static mobj prim_vector_p(mobj args)     { return Mboolean(Mvectorp(Mcar(args))); }
static mobj prim_char_p(mobj args)       { return Mboolean(Mcharp(Mcar(args))); }
static mobj prim_eof_object_p(mobj args) { return Mboolean(Meofp(Mcar(args))); }

/* ----------------------------------------------------------------------
 * Pairs and lists
 * -------------------------------------------------------------------- */

static mobj prim_cons(mobj args) {
    return Mcons(Mcar(args), Mcar(Mcdr(args)));
}
static mobj prim_car(mobj args) { return Mcar(Mcar(args)); }
static mobj prim_cdr(mobj args) { return Mcdr(Mcar(args)); }

/* `args` is already the list the user passed; just return it. The
 * eval loop hands us a freshly-built list, so this is safe to alias. */
static mobj prim_list(mobj args) { return args; }

/* ----------------------------------------------------------------------
 * Vectors
 * -------------------------------------------------------------------- */

static mobj prim_make_vector(mobj args) {
    intptr_t n = Mfixnum_val(Mcar(args));
    mobj rest = Mcdr(args);
    mobj fill = Mpairp(rest) ? Mcar(rest) : Mvoid;
    return Mvector((size_t)n, fill);
}

static mobj prim_vector(mobj args) {
    /* (vector x ...) — allocate then fill. Mvector allocates, so
     * `args` must be protected across the call before we walk it
     * again to fill the slots. */
    size_t n = 0;
    for (mobj p = args; Mpairp(p); p = Mcdr(p)) n++;
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(args);
    mobj v = Mvector(n, Mfalse);
    size_t i = 0;
    for (mobj p = args; Mpairp(p); p = Mcdr(p), i++)
        Mvector_set(v, i, Mcar(p));
    MINIM_GC_FRAME_END;
    return v;
}

static mobj prim_vector_ref(mobj args) {
    return Mvector_ref(Mcar(args),
                       (size_t)Mfixnum_val(Mcar(Mcdr(args))));
}

static mobj prim_vector_set(mobj args) {
    Mvector_set(Mcar(args),
                (size_t)Mfixnum_val(Mcar(Mcdr(args))),
                Mcar(Mcdr(Mcdr(args))));
    return Mvoid;
}

static mobj prim_vector_length(mobj args) {
    return Mfixnum((intptr_t)Mvector_length(Mcar(args)));
}

/* ----------------------------------------------------------------------
 * Equality
 *
 * `eq?` is bare word-equality — fixnums, symbols (interned), immediates,
 * chars, and heap pointers all compare correctly under `==`. R7RS
 * leaves `eq?` on numbers unspecified; word-equality happens to do the
 * "right thing" for fixnums but not for flonums.
 *
 * `eqv?` adds a flonum case so that two flonums with the same numeric
 * value compare equal even when they're distinct heap objects. Other
 * numeric-tower divergences (exact/inexact, bignums) are out of scope.
 * -------------------------------------------------------------------- */

static mobj prim_eq(mobj args) {
    return Mboolean(Mcar(args) == Mcar(Mcdr(args)));
}

static mobj prim_eqv(mobj args) {
    mobj a = Mcar(args);
    mobj b = Mcar(Mcdr(args));
    if (a == b) return Mtrue;
    if (Mflonump(a) && Mflonump(b))
        return Mboolean(Mflonum_val(a) == Mflonum_val(b));
    return Mfalse;
}

static mobj prim_equal(mobj args) {
    return Mboolean(Mequal(Mcar(args), Mcar(Mcdr(args))));
}

static mobj prim_hash(mobj args) {
    /* Mhash returns a 64-bit value; we shift it into a fixnum, which
     * drops the top 3 bits. The fixnum is signed, but downstream
     * hashtable code only needs determinism + equal-implies-equal,
     * not a particular sign convention. */
    return Mfixnum((intptr_t)Mhash(Mcar(args)));
}

/* ----------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------- */

void prims_register_all(void) {
    /* Type predicates */
    prim_register("pair?",       prim_pair_p,       1,  1);
    prim_register("null?",       prim_null_p,       1,  1);
    prim_register("symbol?",     prim_symbol_p,     1,  1);
    prim_register("boolean?",    prim_boolean_p,    1,  1);
    prim_register("number?",     prim_number_p,     1,  1);
    prim_register("procedure?",  prim_procedure_p,  1,  1);
    prim_register("vector?",     prim_vector_p,     1,  1);
    prim_register("char?",       prim_char_p,       1,  1);
    prim_register("eof-object?", prim_eof_object_p, 1,  1);

    /* Pairs / lists */
    prim_register("cons",        prim_cons,         2,  2);
    prim_register("car",         prim_car,          1,  1);
    prim_register("cdr",         prim_cdr,          1,  1);
    prim_register("list",        prim_list,         0, -1);

    /* Vectors */
    prim_register("make-vector",   prim_make_vector,   1,  2);
    prim_register("vector",        prim_vector,        0, -1);
    prim_register("vector-ref",    prim_vector_ref,    2,  2);
    prim_register("vector-set!",   prim_vector_set,    3,  3);
    prim_register("vector-length", prim_vector_length, 1,  1);

    /* Equality + hashing */
    prim_register("eq?",    prim_eq,    2, 2);
    prim_register("eqv?",   prim_eqv,   2, 2);
    prim_register("equal?", prim_equal, 2, 2);
    prim_register("hash",   prim_hash,  1, 1);
}
