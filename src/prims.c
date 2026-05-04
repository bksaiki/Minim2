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
 * Arithmetic
 *
 * Minimal — no type-checking. Mixed fixnum/flonum follows R7RS
 * contagion: if any arg is a flonum, compute in doubles and return
 * a flonum; otherwise stay in fixnum. Overflow wraps in C; division
 * by zero is undefined behavior at this layer.
 *
 * Allocation discipline: each prim reads its args before calling
 * `Mflonum` (the only allocator on the arithmetic path), so `args`
 * is dead by the time the GC could move it. No protection needed.
 * -------------------------------------------------------------------- */

static bool any_flonum(mobj args) {
    for (mobj p = args; Mpairp(p); p = Mcdr(p))
        if (Mflonump(Mcar(p))) return true;
    return false;
}

static double to_double(mobj v) {
    return Mfixnump(v) ? ((double) Mfixnum_val(v)) : Mflonum_val(v);
}

static mobj prim_add(mobj args) {
    if (any_flonum(args)) {
        double s = 0.0;
        for (mobj p = args; Mpairp(p); p = Mcdr(p))
            s += to_double(Mcar(p));
        return Mflonum(s);
    }
    intptr_t s = 0;
    for (mobj p = args; Mpairp(p); p = Mcdr(p))
        s += Mfixnum_val(Mcar(p));
    return Mfixnum(s);
}

static mobj prim_mul(mobj args) {
    if (any_flonum(args)) {
        double r = 1.0;
        for (mobj p = args; Mpairp(p); p = Mcdr(p))
            r *= to_double(Mcar(p));
        return Mflonum(r);
    }
    intptr_t r = 1;
    for (mobj p = args; Mpairp(p); p = Mcdr(p))
        r *= Mfixnum_val(Mcar(p));
    return Mfixnum(r);
}

static mobj prim_sub(mobj args) {
    /* (- x) negates; (- x y z ...) left-folds with subtraction. */
    mobj first = Mcar(args);
    mobj rest = Mcdr(args);
    if (Mnullp(rest)) {
        if (Mflonump(first)) return Mflonum(-Mflonum_val(first));
        return Mfixnum(-Mfixnum_val(first));
    }
    if (Mflonump(first) || any_flonum(rest)) {
        double r = to_double(first);
        for (mobj p = rest; Mpairp(p); p = Mcdr(p))
            r -= to_double(Mcar(p));
        return Mflonum(r);
    }
    intptr_t r = Mfixnum_val(first);
    for (mobj p = rest; Mpairp(p); p = Mcdr(p))
        r -= Mfixnum_val(Mcar(p));
    return Mfixnum(r);
}

/* Pairwise comparison shared by =, <, >, <=, >=. The fixnum branch
 * keeps integer precision; the flonum branch promotes both sides to
 * double once any flonum shows up. */
typedef bool (*cmp_int_fn)(intptr_t, intptr_t);
typedef bool (*cmp_flo_fn)(double, double);

static mobj cmp_pairwise(mobj args, cmp_int_fn ci, cmp_flo_fn cf) {
    if (any_flonum(args)) {
        double prev = to_double(Mcar(args));
        for (mobj p = Mcdr(args); Mpairp(p); p = Mcdr(p)) {
            double cur = to_double(Mcar(p));
            if (!cf(prev, cur)) return Mfalse;
            prev = cur;
        }
        return Mtrue;
    }
    intptr_t prev = Mfixnum_val(Mcar(args));
    for (mobj p = Mcdr(args); Mpairp(p); p = Mcdr(p)) {
        intptr_t cur = Mfixnum_val(Mcar(p));
        if (!ci(prev, cur)) return Mfalse;
        prev = cur;
    }
    return Mtrue;
}

static bool ci_eq(intptr_t a, intptr_t b) { return a == b; }
static bool ci_lt(intptr_t a, intptr_t b) { return a <  b; }
static bool ci_gt(intptr_t a, intptr_t b) { return a >  b; }
static bool ci_le(intptr_t a, intptr_t b) { return a <= b; }
static bool ci_ge(intptr_t a, intptr_t b) { return a >= b; }
static bool cf_eq(double a, double b)     { return a == b; }
static bool cf_lt(double a, double b)     { return a <  b; }
static bool cf_gt(double a, double b)     { return a >  b; }
static bool cf_le(double a, double b)     { return a <= b; }
static bool cf_ge(double a, double b)     { return a >= b; }

static mobj prim_num_eq(mobj args) { return cmp_pairwise(args, ci_eq, cf_eq); }
static mobj prim_lt(mobj args)     { return cmp_pairwise(args, ci_lt, cf_lt); }
static mobj prim_gt(mobj args)     { return cmp_pairwise(args, ci_gt, cf_gt); }
static mobj prim_le(mobj args)     { return cmp_pairwise(args, ci_le, cf_le); }
static mobj prim_ge(mobj args)     { return cmp_pairwise(args, ci_ge, cf_ge); }

/* C99 signed `/` and `%` truncate toward zero, which matches R7RS
 * `quotient`/`remainder` directly. Integer-only — flonum args here
 * are out of scope (R7RS uses these names for integer division). */
static mobj prim_quotient(mobj args) {
    intptr_t a = Mfixnum_val(Mcar(args));
    intptr_t b = Mfixnum_val(Mcar(Mcdr(args)));
    return Mfixnum(a / b);
}
static mobj prim_remainder(mobj args) {
    intptr_t a = Mfixnum_val(Mcar(args));
    intptr_t b = Mfixnum_val(Mcar(Mcdr(args)));
    return Mfixnum(a % b);
}

static mobj prim_zero_p(mobj args) {
    mobj v = Mcar(args);
    if (Mflonump(v)) return Mboolean(Mflonum_val(v) == 0.0);
    return Mboolean(Mfixnum_val(v) == 0);
}
static mobj prim_positive_p(mobj args) {
    mobj v = Mcar(args);
    if (Mflonump(v)) return Mboolean(Mflonum_val(v) > 0.0);
    return Mboolean(Mfixnum_val(v) > 0);
}
static mobj prim_negative_p(mobj args) {
    mobj v = Mcar(args);
    if (Mflonump(v)) return Mboolean(Mflonum_val(v) < 0.0);
    return Mboolean(Mfixnum_val(v) < 0);
}
static mobj prim_abs(mobj args) {
    mobj v = Mcar(args);
    if (Mflonump(v)) {
        double d = Mflonum_val(v);
        return d < 0.0 ? Mflonum(-d) : v;
    }
    intptr_t n = Mfixnum_val(v);
    return n < 0 ? Mfixnum(-n) : v;
}

/* Fixnum-to-flonum bridge. Until the reader recognizes flonum
 * literals, this is the only Scheme-level path to a flonum value —
 * lets tests exercise the contagion logic above. Identity on flonum
 * input matches R7RS. */
static mobj prim_exact_to_inexact(mobj args) {
    mobj v = Mcar(args);
    if (Mflonump(v)) return v;
    return Mflonum((double)Mfixnum_val(v));
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

    /* Arithmetic */
    prim_register("+",              prim_add,              0, -1);
    prim_register("-",              prim_sub,              1, -1);
    prim_register("*",              prim_mul,              0, -1);
    prim_register("=",              prim_num_eq,           2, -1);
    prim_register("<",              prim_lt,               2, -1);
    prim_register(">",              prim_gt,               2, -1);
    prim_register("<=",             prim_le,               2, -1);
    prim_register(">=",             prim_ge,               2, -1);
    prim_register("quotient",       prim_quotient,         2,  2);
    prim_register("remainder",      prim_remainder,        2,  2);
    prim_register("zero?",          prim_zero_p,           1,  1);
    prim_register("positive?",      prim_positive_p,       1,  1);
    prim_register("negative?",      prim_negative_p,       1,  1);
    prim_register("abs",            prim_abs,              1,  1);
    prim_register("exact->inexact", prim_exact_to_inexact, 1,  1);

    /* Equality + hashing */
    prim_register("eq?",    prim_eq,    2, 2);
    prim_register("eqv?",   prim_eqv,   2, 2);
    prim_register("equal?", prim_equal, 2, 2);
    prim_register("hash",   prim_hash,  1, 1);
}
