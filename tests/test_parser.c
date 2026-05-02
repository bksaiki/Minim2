#include "minim.h"
#include "parser.h"
#include "harness.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------- */

static mobj read_one(const char *src) {
    mreader r;
    mreader_init_string(&r, src);
    return Mread(&r);
}

/* ----------------------------------------------------------------------
 * Atoms
 * -------------------------------------------------------------------- */

static void test_immediates(void) {
    Minit();
    CHECK(Mtruep(read_one("#t")), "read: #t");
    CHECK(Mfalsep(read_one("#f")), "read: #f");
    CHECK(Mnullp(read_one("()")), "read: ()");

    /* EOF on empty input */
    CHECK(Meofp(read_one("")), "read: empty -> eof");
    CHECK(Meofp(read_one("   ")), "read: only whitespace -> eof");
    CHECK(Meofp(read_one("; just a comment\n")), "read: only comment -> eof");
    Mshutdown();
}

static void test_fixnums(void) {
    Minit();
    mobj v = read_one("42");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 42, "fixnum: 42");

    v = read_one("-7");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == -7, "fixnum: -7");

    v = read_one("+3");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 3, "fixnum: +3");

    v = read_one("0");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 0, "fixnum: 0");

    /* Hex */
    v = read_one("#x10");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 16, "fixnum: #x10 = 16");

    v = read_one("#xff");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 255, "fixnum: #xff = 255");

    v = read_one("#xFF");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 255, "fixnum: #xFF = 255");

    v = read_one("#x-10");
    CHECK(Mfixnump(v) && Mfixnum_val(v) == -16, "fixnum: #x-10 = -16");
    Mshutdown();
}

static void test_symbols(void) {
    Minit();
    mobj a = read_one("foo");
    CHECK(Msymbolp(a), "symbol: foo is symbol");
    CHECK(strcmp(Msymbol_name(a), "foo") == 0, "symbol: foo name");

    mobj b = read_one("foo");
    CHECK(a == b, "symbol: foo interned identically");

    mobj plus = read_one("+");
    CHECK(Msymbolp(plus), "symbol: + alone");
    CHECK(strcmp(Msymbol_name(plus), "+") == 0, "symbol: + name");

    mobj minus = read_one("-");
    CHECK(Msymbolp(minus), "symbol: - alone");

    mobj sys = read_one("#%plain-app");
    CHECK(Msymbolp(sys), "symbol: #%plain-app");
    CHECK(strcmp(Msymbol_name(sys), "#%plain-app") == 0,
          "symbol: #%plain-app name");

    /* `1foo` starts with a digit but contains a non-digit, so it falls
     * through to the symbol path. */
    mobj weird = read_one("1foo");
    CHECK(Msymbolp(weird), "symbol: 1foo is symbol (mixed)");
    CHECK(strcmp(Msymbol_name(weird), "1foo") == 0, "symbol: 1foo name");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Lists, dotted pairs, quote
 * -------------------------------------------------------------------- */

static void test_simple_list(void) {
    Minit();
    mobj v = read_one("(1 2 3)");
    CHECK(Mpairp(v), "list: (1 2 3) is pair");
    CHECK(Mfixnum_val(Mcar(v)) == 1, "list: car=1");
    CHECK(Mfixnum_val(Mcar(Mcdr(v))) == 2, "list: cadr=2");
    CHECK(Mfixnum_val(Mcar(Mcdr(Mcdr(v)))) == 3, "list: caddr=3");
    CHECK(Mnullp(Mcdr(Mcdr(Mcdr(v)))), "list: terminates with ()");
    Mshutdown();
}

static void test_brackets(void) {
    Minit();
    mobj v = read_one("[1 2]");
    CHECK(Mpairp(v) && Mfixnum_val(Mcar(v)) == 1, "list: [1 2] car=1");

    v = read_one("{a b c}");
    CHECK(Mpairp(v) && Msymbolp(Mcar(v)), "list: {a b c} car symbol");
    Mshutdown();
}

static void test_dotted_pair(void) {
    Minit();
    mobj v = read_one("(1 . 2)");
    CHECK(Mpairp(v), "dotted: pair");
    CHECK(Mfixnum_val(Mcar(v)) == 1, "dotted: car=1");
    CHECK(Mfixnum_val(Mcdr(v)) == 2, "dotted: cdr=2");

    /* (1 2 . 3) — list with last cdr non-nil */
    v = read_one("(1 2 . 3)");
    CHECK(Mpairp(v), "dotted: (1 2 . 3)");
    CHECK(Mfixnum_val(Mcar(v)) == 1, "dotted: car=1");
    CHECK(Mfixnum_val(Mcar(Mcdr(v))) == 2, "dotted: cadr=2");
    CHECK(Mfixnum_val(Mcdr(Mcdr(v))) == 3, "dotted: cddr=3");
    Mshutdown();
}

static void test_nested_list(void) {
    Minit();
    mobj v = read_one("(1 (2 3) (4 (5 6)))");
    CHECK(Mpairp(v), "nested: outer pair");
    mobj inner = Mcar(Mcdr(v));
    CHECK(Mpairp(inner) && Mfixnum_val(Mcar(inner)) == 2,
          "nested: cadr is (2 3)");
    Mshutdown();
}

static void test_quote(void) {
    Minit();
    mobj v = read_one("'foo");
    CHECK(Mpairp(v), "quote: 'foo -> pair");

    mobj head = Mcar(v);
    CHECK(Msymbolp(head) && strcmp(Msymbol_name(head), "quote") == 0,
          "quote: car is `quote` symbol");

    mobj inner = Mcar(Mcdr(v));
    CHECK(Msymbolp(inner) && strcmp(Msymbol_name(inner), "foo") == 0,
          "quote: cadr is foo");

    CHECK(Mnullp(Mcdr(Mcdr(v))), "quote: properly terminated");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Vectors
 * -------------------------------------------------------------------- */

static void test_vector(void) {
    Minit();
    mobj v = read_one("#(1 2 3)");
    CHECK(Mvectorp(v), "vector: #(1 2 3)");
    CHECK(Mvector_length(v) == 3, "vector: length 3");
    CHECK(Mfixnum_val(Mvector_ref(v, 0)) == 1, "vector: [0]=1");
    CHECK(Mfixnum_val(Mvector_ref(v, 1)) == 2, "vector: [1]=2");
    CHECK(Mfixnum_val(Mvector_ref(v, 2)) == 3, "vector: [2]=3");

    /* Empty */
    v = read_one("#()");
    CHECK(Mvectorp(v) && Mvector_length(v) == 0, "vector: empty");

    /* Mixed types incl. nested list */
    v = read_one("#(1 foo (2 3))");
    CHECK(Mvectorp(v) && Mvector_length(v) == 3, "vector: mixed length 3");
    CHECK(Mfixnum_val(Mvector_ref(v, 0)) == 1, "vector: mixed [0]=1");
    CHECK(Msymbolp(Mvector_ref(v, 1)), "vector: mixed [1]=symbol");
    CHECK(Mpairp(Mvector_ref(v, 2)), "vector: mixed [2]=pair");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Comments
 * -------------------------------------------------------------------- */

static void test_comments(void) {
    Minit();
    mobj v = read_one("; line\n42");
    CHECK(Mfixnum_val(v) == 42, "comment: line skipped");

    v = read_one("#| block |# 7");
    CHECK(Mfixnum_val(v) == 7, "comment: block skipped");

    v = read_one("#| nested #| inner |# still in outer |# 9");
    CHECK(Mfixnum_val(v) == 9, "comment: nested block skipped");

    v = read_one("#;42 99");
    CHECK(Mfixnum_val(v) == 99, "comment: datum comment skips 42");

    v = read_one("(1 #;2 3)");
    CHECK(Mpairp(v), "comment: list with datum comment");
    CHECK(Mfixnum_val(Mcar(v)) == 1, "comment: list cadr=1");
    CHECK(Mfixnum_val(Mcar(Mcdr(v))) == 3, "comment: list cadr=3 (2 dropped)");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Multiple datums from one stream
 * -------------------------------------------------------------------- */

static void test_multiple_datums(void) {
    Minit();
    mreader r;
    mreader_init_string(&r, "1 2 (3 4)");
    mobj a = Mread(&r);
    mobj b = Mread(&r);
    mobj c = Mread(&r);
    mobj d = Mread(&r);
    CHECK(Mfixnum_val(a) == 1, "multi: first=1");
    CHECK(Mfixnum_val(b) == 2, "multi: second=2");
    CHECK(Mpairp(c), "multi: third is pair");
    CHECK(Meofp(d), "multi: fourth is eof");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * GC interaction — deeply nested allocation must not lose data
 * -------------------------------------------------------------------- */

static void test_parser_allocates_safely(void) {
    Minit();
    /* A long list forces many allocations; the parser's protect frames
     * must keep head/tail/x alive across each Mcons. */
    char buf[2048];
    size_t pos = 0;
    buf[pos++] = '(';
    for (int i = 0; i < 200; i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%d ", i);
        pos += (size_t)n;
    }
    buf[pos++] = ')';
    buf[pos] = '\0';

    mobj v = read_one(buf);
    int ok = 1;
    int expected = 0;
    for (mobj cur = v; !Mnullp(cur); cur = Mcdr(cur)) {
        if (!Mpairp(cur) || Mfixnum_val(Mcar(cur)) != expected) {
            ok = 0;
            break;
        }
        expected++;
    }
    CHECK(ok, "parse-with-gc: list values intact");
    CHECK(expected == 200, "parse-with-gc: list length=200");
    Mshutdown();
}

int main(void) {
    test_immediates();
    test_fixnums();
    test_symbols();
    test_simple_list();
    test_brackets();
    test_dotted_pair();
    test_nested_list();
    test_quote();
    test_vector();
    test_comments();
    test_multiple_datums();
    test_parser_allocates_safely();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
