#include "minim.h"
#include "writer.h"

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
 * Top-level dispatch
 * -------------------------------------------------------------------- */

void Mwrite(mobj v, FILE *out) {
    if (Mnullp(v))   { fputs("()", out);     return; }
    if (Mtruep(v))   { fputs("#t", out);     return; }
    if (Mfalsep(v))  { fputs("#f", out);     return; }
    if (Meofp(v))    { fputs("#<eof>", out); return; }

    if (Mfixnump(v)) {
        fprintf(out, "%" PRIdPTR, (intptr_t)Mfixnum_val(v));
        return;
    }
    if (Mflonump(v)) { write_flonum(v, out); return; }
    if (Msymbolp(v)) { fputs(Msymbol_name(v), out); return; }
    if (Mpairp(v))   { write_pair(v, out);   return; }
    if (Mvectorp(v)) { write_vector(v, out); return; }

    /* Should not reach here in v1 — every primary tag has a clause. */
    fputs("#<garbage>", out);
}
