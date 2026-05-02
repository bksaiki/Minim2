#define _GNU_SOURCE  /* open_memstream */

#include "minim.h"
#include "parser.h"
#include "writer.h"
#include "harness.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------- */

/* Returns a malloc'd null-terminated string with the writer output.
 * Caller frees. */
static char *write_to_str(mobj v) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) { perror("open_memstream"); abort(); }
    Mwrite(v, f);
    fflush(f);
    fclose(f);
    return buf;
}

/* Read one datum from `src`, write it back, return the string. */
static char *roundtrip(const char *src) {
    mreader r;
    mreader_init_string(&r, src);
    mobj v = Mread(&r);
    return write_to_str(v);
}

#define CHECK_WRITE(expr, expected, name) do {           \
    char *_s = write_to_str(expr);                       \
    CHECK(strcmp(_s, (expected)) == 0, name);            \
    if (strcmp(_s, (expected)) != 0)                     \
        fprintf(stderr, "  got: \"%s\"\n  want: \"%s\"\n", _s, (expected)); \
    free(_s);                                            \
} while (0)

#define CHECK_RT(input, expected, name) do {             \
    char *_s = roundtrip(input);                         \
    CHECK(strcmp(_s, (expected)) == 0, name);            \
    if (strcmp(_s, (expected)) != 0)                     \
        fprintf(stderr, "  input: \"%s\"\n  got: \"%s\"\n  want: \"%s\"\n", \
                (input), _s, (expected));                \
    free(_s);                                            \
} while (0)

/* ----------------------------------------------------------------------
 * Atoms
 * -------------------------------------------------------------------- */

static void test_immediates(void) {
    Minit();
    CHECK_WRITE(Mtrue,  "#t",     "write: #t");
    CHECK_WRITE(Mfalse, "#f",     "write: #f");
    CHECK_WRITE(Mnull,  "()",     "write: ()");
    CHECK_WRITE(Meof,   "#<eof>", "write: #<eof>");
    Mshutdown();
}

static void test_fixnums(void) {
    Minit();
    CHECK_WRITE(Mfixnum(0),    "0",   "write: 0");
    CHECK_WRITE(Mfixnum(42),   "42",  "write: 42");
    CHECK_WRITE(Mfixnum(-7),   "-7",  "write: -7");

    /* Largest representable fixnum on a 64-bit host. */
    intptr_t big = ((intptr_t)1 << 60) - 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIdPTR, big);
    CHECK_WRITE(Mfixnum(big), buf, "write: max fixnum");
    Mshutdown();
}

static void test_flonums(void) {
    Minit();
    /* Integer-valued doubles get the `.0` suffix to stay readable as flonum. */
    CHECK_WRITE(Mflonum(1.0),  "1.0",   "write: 1.0");
    CHECK_WRITE(Mflonum(-2.0), "-2.0",  "write: -2.0");
    CHECK_WRITE(Mflonum(0.5),  "0.5",   "write: 0.5");

    /* Special values follow R7RS spelling. */
    double inf = 1.0 / 0.0;
    CHECK_WRITE(Mflonum(inf),  "+inf.0", "write: +inf.0");
    CHECK_WRITE(Mflonum(-inf), "-inf.0", "write: -inf.0");

    double nan = 0.0 / 0.0;
    CHECK_WRITE(Mflonum(nan),  "+nan.0", "write: +nan.0");

    /* A non-integral value: writer must produce a string that contains
     * `.` (so it is not confused with a fixnum on the wire). Read-back
     * round-tripping is deferred until the parser learns flonum syntax
     * — see docs/agents/parser.md Phase 3. */
    char *s = write_to_str(Mflonum(3.14159));
    CHECK(strchr(s, '.') != NULL, "flonum: 3.14159 contains '.'");
    free(s);
    Mshutdown();
}

static void test_symbols(void) {
    Minit();
    CHECK_WRITE(Mintern("foo"),       "foo",       "write: foo");
    CHECK_WRITE(Mintern("hello-world"),"hello-world","write: hello-world");
    CHECK_WRITE(Mintern("+"),         "+",         "write: +");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Compound: lists, vectors
 * -------------------------------------------------------------------- */

static void test_lists(void) {
    Minit();
    /* Proper list (1 2 3) */
    mobj a = Mcons(Mfixnum(1), Mcons(Mfixnum(2), Mcons(Mfixnum(3), Mnull)));
    CHECK_WRITE(a, "(1 2 3)", "write: (1 2 3)");

    /* Improper: (1 . 2) */
    mobj b = Mcons(Mfixnum(1), Mfixnum(2));
    CHECK_WRITE(b, "(1 . 2)", "write: (1 . 2)");

    /* Improper longer: (1 2 . 3) */
    mobj c = Mcons(Mfixnum(1), Mcons(Mfixnum(2), Mfixnum(3)));
    CHECK_WRITE(c, "(1 2 . 3)", "write: (1 2 . 3)");

    /* Nested */
    mobj d = Mcons(Mfixnum(1),
                   Mcons(Mcons(Mfixnum(2), Mcons(Mfixnum(3), Mnull)),
                         Mnull));
    CHECK_WRITE(d, "(1 (2 3))", "write: (1 (2 3))");
    Mshutdown();
}

static void test_vectors(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj v = Mvector(0, Mfalse);
    MINIM_GC_PROTECT(v);
    CHECK_WRITE(v, "#()", "write: empty vector");

    v = Mvector(3, Mfixnum(0));
    Mvector_set(v, 0, Mfixnum(1));
    Mvector_set(v, 1, Mfixnum(2));
    Mvector_set(v, 2, Mfixnum(3));
    CHECK_WRITE(v, "#(1 2 3)", "write: #(1 2 3)");

    /* Vector with mixed values — w must be protected because Mintern
     * and Mcons can both trigger a collection that moves it. */
    mobj w = Mvector(3, Mfalse);
    MINIM_GC_PROTECT(w);
    Mvector_set(w, 0, Mintern("a"));
    Mvector_set(w, 1, Mtrue);
    Mvector_set(w, 2, Mcons(Mfixnum(1), Mnull));
    CHECK_WRITE(w, "#(a #t (1))", "write: #(a #t (1))");
    MINIM_GC_FRAME_END;
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Round-trip: parse then write should reproduce a canonical form
 * -------------------------------------------------------------------- */

static void test_roundtrip(void) {
    Minit();
    CHECK_RT("42",                "42",            "rt: fixnum");
    CHECK_RT("-7",                "-7",            "rt: neg fixnum");
    CHECK_RT("#x10",              "16",            "rt: hex normalizes to decimal");
    CHECK_RT("foo",               "foo",           "rt: symbol");
    CHECK_RT("(1 2 3)",           "(1 2 3)",       "rt: simple list");
    CHECK_RT("[1 2 3]",           "(1 2 3)",       "rt: brackets normalize to parens");
    CHECK_RT("(a . b)",           "(a . b)",       "rt: dotted pair");
    CHECK_RT("(1 2 . 3)",         "(1 2 . 3)",     "rt: improper list");
    CHECK_RT("#(1 2 3)",          "#(1 2 3)",      "rt: vector");
    CHECK_RT("#()",               "#()",           "rt: empty vector");
    CHECK_RT("'foo",              "(quote foo)",   "rt: quote desugar");
    CHECK_RT("(a (b c) d)",       "(a (b c) d)",   "rt: nested list");
    CHECK_RT("()",                "()",            "rt: empty list");
    CHECK_RT("#t",                "#t",            "rt: #t");
    CHECK_RT("#f",                "#f",            "rt: #f");

    /* Comments and whitespace are dropped on read; written form is the
     * canonical compact one. */
    CHECK_RT("; comment\n42",     "42",            "rt: leading line comment");
    CHECK_RT("#| block |# 7",     "7",             "rt: block comment");
    CHECK_RT("(1 #;2 3)",         "(1 3)",         "rt: datum comment in list");
    Mshutdown();
}

int main(void) {
    test_immediates();
    test_fixnums();
    test_flonums();
    test_symbols();
    test_lists();
    test_vectors();
    test_roundtrip();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
