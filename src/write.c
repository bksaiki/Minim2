#include "minim.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------------
 * `Mwrite` (canonical / readable) and `Mdisplay` (raw) share their
 * implementation: the same recursive walker, parameterized on a mode
 * flag. Only the string and character arms diverge. Every other type
 * — pairs, vectors, numbers, immediates, procedures — produces the
 * same output regardless of mode.
 * -------------------------------------------------------------------- */

typedef enum {
    WRITER_WRITE,
    WRITER_DISPLAY,
} writer_mode;

static void write_to(mobj v, FILE *out, writer_mode mode);

/* ----------------------------------------------------------------------
 * Pair / list — recurses with the outer mode so that
 * `(display (list "hi" #\a))` prints `(hi a)` and the corresponding
 * `(write ...)` prints `("hi" #\a)`.
 * -------------------------------------------------------------------- */

static void write_pair(mobj v, FILE *out, writer_mode mode) {
    fputc('(', out);
    write_to(Mcar(v), out, mode);
    mobj cur = Mcdr(v);
    while (Mpairp(cur)) {
        fputc(' ', out);
        write_to(Mcar(cur), out, mode);
        cur = Mcdr(cur);
    }
    if (!Mnullp(cur)) {
        fputs(" . ", out);
        write_to(cur, out, mode);
    }
    fputc(')', out);
}

/* ----------------------------------------------------------------------
 * Vector
 * -------------------------------------------------------------------- */

static void write_vector(mobj v, FILE *out, writer_mode mode) {
    size_t len = Mvector_length(v);
    if (len == 0) {
        fputs("#()", out);
        return;
    }
    fputs("#(", out);
    write_to(Mvector_ref(v, 0), out, mode);
    for (size_t i = 1; i < len; i++) {
        fputc(' ', out);
        write_to(Mvector_ref(v, i), out, mode);
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
 * Character
 *
 * Write mode is canonical: 9 R7RS named chars get their `#\<name>`
 * spelling, printable ASCII gets `#\<single>`, everything else goes
 * out as `#\x<hex>` with no leading zeros. The reader accepts the same
 * set; round-trips are stable.
 *
 * Display mode emits the raw byte for codepoints up to 0x7F (the v1
 * ASCII range). Codepoints beyond that fall back to the canonical hex
 * form — there's no single-byte representation for them, and v1
 * doesn't have UTF-8 string support to do otherwise.
 * -------------------------------------------------------------------- */

static void write_char(mobj v, FILE *out, writer_mode mode) {
    mchar c = Mchar_val(v);
    if (mode == WRITER_DISPLAY) {
        if (c <= 0x7F) {
            fputc((int)c, out);
            return;
        }
        /* Fall through to canonical form below. */
    }
    switch (c) {
    case 0x00: fputs("#\\null",      out); return;
    case 0x07: fputs("#\\alarm",     out); return;
    case 0x08: fputs("#\\backspace", out); return;
    case 0x09: fputs("#\\tab",       out); return;
    case 0x0A: fputs("#\\newline",   out); return;
    case 0x0D: fputs("#\\return",    out); return;
    case 0x1B: fputs("#\\escape",    out); return;
    case 0x20: fputs("#\\space",     out); return;
    case 0x7F: fputs("#\\delete",    out); return;
    }
    if (c >= 0x21 && c <= 0x7E) {
        /* Printable ASCII (excluding space, which is named above). */
        fputc('#', out);
        fputc('\\', out);
        fputc((int)c, out);
        return;
    }
    /* Hex form, unpadded. The reader accepts variable-length hex
     * digits; matching that here keeps `#\xC` etc. minimal. */
    fprintf(out, "#\\x%X", (unsigned int)c);
}

/* ----------------------------------------------------------------------
 * String
 *
 * Write mode wraps the content in `"..."` and escapes `\\ \" \n \t \r`
 * — the four escapes the reader accepts, exactly inverted. v1 strings
 * are ASCII-only (the reader rejects bytes >= 0x80), so any other byte
 * 0x00..0x7F goes out raw. Round-trip works because the reader treats
 * non-special bytes as literal content.
 *
 * Display mode emits the raw bytes verbatim, which is the whole point
 * of the write/display split.
 * -------------------------------------------------------------------- */

static void write_string(mobj v, FILE *out, writer_mode mode) {
    size_t len = Mstring_length(v);
    const char *bytes = Mstring_bytes(v);
    if (mode == WRITER_DISPLAY) {
        if (len > 0) fwrite(bytes, 1, len, out);
        return;
    }
    fputc('"', out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        switch (c) {
        case '\\': fputs("\\\\", out); break;
        case '"':  fputs("\\\"", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\t': fputs("\\t",  out); break;
        case '\r': fputs("\\r",  out); break;
        default:   fputc((int)c, out); break;
        }
    }
    fputc('"', out);
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

static void write_to(mobj v, FILE *out, writer_mode mode) {
    switch (v & MTAG_MASK) {
    case MTAG_FIXNUM:
        fprintf(out, "%" PRIdPTR, (intptr_t)Mfixnum_val(v));
        return;
    case MTAG_PAIR:
        write_pair(v, out, mode);
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
        /* Chars are immediates whose low byte is MCHAR_TAG; check
         * before the constant-equality switch so character values
         * reach the right writer. */
        if (Mcharp(v)) { write_char(v, out, mode); return; }
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
        case MSEC_VECTOR: write_vector(v, out, mode); return;
        case MSEC_STRING: write_string(v, out, mode); return;
        case MSEC_KONT:   fputs("#<continuation>", out); return;
        case MSEC_ENV:    fputs("#<environment>", out); return;
        case MSEC_PRIM:   write_named_procedure(Mprim_name(v), out); return;
        }
    }
    }

    /* Reserved primary tag (0x4) or a corrupt value. Should not
     * reach here in v1. */
    fputs("#<garbage>", out);
}

void Mwrite(mobj v, FILE *out) {
    write_to(v, out, WRITER_WRITE);
}

void Mdisplay(mobj v, FILE *out) {
    write_to(v, out, WRITER_DISPLAY);
}
