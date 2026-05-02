#include "minim.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------------
 * Pair / list
 * -------------------------------------------------------------------- */

static void write_pair(mobj v, FILE *out) {
    fputc('(', out);
    Mwrite(Mcar(v), out);
    mobj cur = Mcdr(v);
    while (Mpairp(cur)) {
        fputc(' ', out);
        Mwrite(Mcar(cur), out);
        cur = Mcdr(cur);
    }
    if (!Mnullp(cur)) {
        fputs(" . ", out);
        Mwrite(cur, out);
    }
    fputc(')', out);
}

/* ----------------------------------------------------------------------
 * Vector
 * -------------------------------------------------------------------- */

static void write_vector(mobj v, FILE *out) {
    size_t len = Mvector_length(v);
    if (len == 0) {
        fputs("#()", out);
        return;
    }
    fputs("#(", out);
    Mwrite(Mvector_ref(v, 0), out);
    for (size_t i = 1; i < len; i++) {
        fputc(' ', out);
        Mwrite(Mvector_ref(v, i), out);
    }
    fputc(')', out);
}

/* ----------------------------------------------------------------------
 * Flonum
 *
 * `%.17g` prints any finite double in a form that round-trips through
 * strtod. If the result has no `.` or `e`, the value is integer-valued
 * (e.g. `1`) — we append `.0` to keep flonum/fixnum distinct on the
 * page. Special values use the R7RS spelling.
 * -------------------------------------------------------------------- */

static void write_flonum(mobj v, FILE *out) {
    double d = Mflonum_val(v);
    if (isnan(d))      { fputs("+nan.0", out); return; }
    if (isinf(d))      { fputs(d < 0 ? "-inf.0" : "+inf.0", out); return; }

    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", d);
    fputs(buf, out);
    if (!strpbrk(buf, ".eE")) fputs(".0", out);
}

/* ----------------------------------------------------------------------
 * Procedures
 *
 * Closures, primitives, and continuations all print as
 * `#<procedure>` (or `#<procedure:name>` when a name is on file).
 * The output is unreadable — there is no source-level syntax for a
 * runtime procedure value — but it lets `(write some-closure)`
 * produce something sensible instead of `#<garbage>`.
 * -------------------------------------------------------------------- */

static void write_named_procedure(mobj name, FILE *out) {
    if (Msymbolp(name)) {
        fputs("#<procedure:", out);
        fputs(Msymbol_name(name), out);
        fputc('>', out);
    } else {
        fputs("#<procedure>", out);
    }
}

/* ----------------------------------------------------------------------
 * Top-level dispatch
 *
 * Two-level switch: primary tag first (one mask + jump), then a
 * nested switch on the secondary tag for typed objects. Beats a
 * chain of predicate calls because each predicate would re-mask the
 * tag, and several of the typed-obj predicates also dereference the
 * header redundantly.
 * -------------------------------------------------------------------- */

void Mwrite(mobj v, FILE *out) {
    switch (v & MTAG_MASK) {
    case MTAG_FIXNUM:
        fprintf(out, "%" PRIdPTR, (intptr_t)Mfixnum_val(v));
        return;
    case MTAG_PAIR:
        write_pair(v, out);
        return;
    case MTAG_FLONUM:
        write_flonum(v, out);
        return;
    case MTAG_SYMBOL:
        fputs(Msymbol_name(v), out);
        return;
    case MTAG_CLOSURE:
        write_named_procedure(Mclosure_name(v), out);
        return;
    case MTAG_IMMEDIATE:
        switch (v) {
        case Mfalse: fputs("#f", out);      return;
        case Mtrue:  fputs("#t", out);      return;
        case Mnull:  fputs("()", out);      return;
        case Meof:   fputs("#<eof>", out);  return;
        case Mvoid:  fputs("#<void>", out); return;
        default:     fputs("#<unknown-immediate>", out); return;
        }
    case MTAG_TYPED_OBJ: {
        mobj header = *((mobj *)((uintptr_t)v - MTAG_TYPED_OBJ));
        switch (header & MSEC_MASK) {
        case MSEC_VECTOR: write_vector(v, out); return;
        case MSEC_KONT:   fputs("#<continuation>", out); return;
        case MSEC_ENV:    fputs("#<environment>", out); return;
        case MSEC_PRIM:   write_named_procedure(Mprim_name(v), out); return;
        }
    }
    }

    /* Reserved primary tag (0x4 — future strings) or a corrupt
     * value. Should not reach here in v1. */
    fputs("#<garbage>", out);
}
