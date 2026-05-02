#include "minim.h"
#include "harness.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Phase 2 — minimal evaluator
 *
 * Self-evaluating literals, quote, if, begin. No lambda, no define,
 * no application yet. Tests parse source via Mread and feed the
 * resulting mobj to Meval; the canonical form covers reader-side
 * round-trip too.
 * -------------------------------------------------------------------- */

static mobj eval_str(const char *src) {
    mreader r;
    mreader_init_string(&r, src);
    mobj expr = Mread(&r);
    return Meval(expr);
}

static void test_self_evaluating(void) {
    Minit();
    CHECK(Mfixnum_val(eval_str("42")) == 42, "self: fixnum 42");
    CHECK(Mfixnum_val(eval_str("-7")) == -7, "self: fixnum -7");
    CHECK(eval_str("#t") == Mtrue,  "self: #t");
    CHECK(eval_str("#f") == Mfalse, "self: #f");
    CHECK(Mvectorp(eval_str("#(1 2 3)")), "self: vector evaluates to itself");

    mobj v = eval_str("#(10 20 30)");
    CHECK(Mvector_length(v) == 3, "self: vector length preserved");
    CHECK(Mfixnum_val(Mvector_ref(v, 0)) == 10, "self: vector[0]=10");
    CHECK(Mfixnum_val(Mvector_ref(v, 2)) == 30, "self: vector[2]=30");
    Mshutdown();
}

static void test_quote(void) {
    Minit();
    /* (quote foo) → foo */
    mobj v = eval_str("(quote foo)");
    CHECK(Msymbolp(v), "quote: foo is symbol");
    CHECK(strcmp(Msymbol_name(v), "foo") == 0, "quote: foo name");

    /* 'foo desugars to (quote foo) at read time. */
    v = eval_str("'foo");
    CHECK(Msymbolp(v) && strcmp(Msymbol_name(v), "foo") == 0, "quote: 'foo");

    /* (quote (a b c)) → list */
    v = eval_str("(quote (a b c))");
    CHECK(Mpairp(v), "quote: list");
    CHECK(Msymbolp(Mcar(v)), "quote: list car symbol");
    CHECK(strcmp(Msymbol_name(Mcar(v)), "a") == 0, "quote: car=a");
    CHECK(strcmp(Msymbol_name(Mcar(Mcdr(v))), "b") == 0, "quote: cadr=b");
    CHECK(strcmp(Msymbol_name(Mcar(Mcdr(Mcdr(v)))), "c") == 0, "quote: caddr=c");
    CHECK(Mnullp(Mcdr(Mcdr(Mcdr(v)))), "quote: list terminated");

    /* (quote ()) → () */
    CHECK(Mnullp(eval_str("(quote ())")), "quote: () → ()");

    /* Numbers and booleans inside a quoted list. */
    v = eval_str("(quote (1 #t 2))");
    CHECK(Mfixnum_val(Mcar(v)) == 1, "quote: nested fixnum");
    CHECK(Mtruep(Mcar(Mcdr(v))), "quote: nested #t");
    Mshutdown();
}

static void test_if(void) {
    Minit();
    CHECK(Mfixnum_val(eval_str("(if #t 1 2)")) == 1, "if: #t branch");
    CHECK(Mfixnum_val(eval_str("(if #f 1 2)")) == 2, "if: #f branch");

    /* Truthy: only #f is false; everything else is true. */
    CHECK(Mfixnum_val(eval_str("(if 0 1 2)")) == 1, "if: 0 is truthy");
    CHECK(Mfixnum_val(eval_str("(if (quote ()) 1 2)")) == 1, "if: () is truthy");
    CHECK(Mfixnum_val(eval_str("(if (quote x) 1 2)")) == 1, "if: symbol is truthy");

    /* Branch values are arbitrary expressions, themselves evaluated. */
    mobj v = eval_str("(if #t (quote then) (quote else))");
    CHECK(Msymbolp(v) && strcmp(Msymbol_name(v), "then") == 0, "if: branch is evaluated");
    Mshutdown();
}

static void test_begin(void) {
    Minit();
    /* (begin e1 ... en) → value of en */
    CHECK(Mfixnum_val(eval_str("(begin 1 2 3)")) == 3, "begin: 1 2 3 → 3");
    CHECK(Mfixnum_val(eval_str("(begin 42)")) == 42, "begin: single expr");

    mobj v = eval_str("(begin 1 2 (quote done))");
    CHECK(Msymbolp(v) && strcmp(Msymbol_name(v), "done") == 0,
          "begin: tail is evaluated");

    /* Body forms are themselves evaluated, and the discarded
     * intermediate values must not break anything. */
    CHECK(Mfixnum_val(eval_str("(begin (if #t 99 0) 1 2)")) == 2,
          "begin: if in non-tail still works");
    Mshutdown();
}

static void test_nested(void) {
    Minit();
    /* if inside begin */
    mobj v = eval_str("(begin (quote a) (if #t (quote yes) (quote no)))");
    CHECK(Msymbolp(v) && strcmp(Msymbol_name(v), "yes") == 0,
          "nested: begin/if/quote");

    /* begin inside if */
    v = eval_str("(if #f 99 (begin 1 2 3))");
    CHECK(Mfixnum_val(v) == 3, "nested: if/begin");

    /* Quote inside if's test position. */
    v = eval_str("(if (quote x) (quote yes) (quote no))");
    CHECK(strcmp(Msymbol_name(v), "yes") == 0, "nested: quoted symbol is truthy");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Tail-position discipline (Phase 6 land, but already verifiable):
 * a deeply nested begin must not accumulate kont frames per
 * iteration, and a deeply nested if's branches must not either.
 *
 * We can't yet measure heap usage directly, but under
 * MINIM_GC_STRESS=ON every allocation triggers a collection — if a
 * frame is pushed but never popped, the chain grows and any single
 * test still succeeds (just slowly). What WE check is correctness:
 * a 1000-deep begin and 1000-deep if-chain still produce the right
 * value. Real heap-bound tests follow once lambdas exist.
 * -------------------------------------------------------------------- */

static void test_deep_begin(void) {
    Minit();
    /* (begin 1 2 3 ... 200 'last) — 201 expressions; final is `last`. */
    char buf[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "(begin");
    for (int i = 0; i < 200; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " %d", i);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " (quote last))");

    mobj v = eval_str(buf);
    CHECK(Msymbolp(v) && strcmp(Msymbol_name(v), "last") == 0,
          "deep begin: tail value still correct");
    Mshutdown();
}

static void test_deep_if(void) {
    Minit();
    /* Build a right-nested if: (if #t (if #t (if #t ... 42 0) 0) 0).
     * Each branch the test takes is the THEN, so the result is 42. */
    char buf[4096];
    size_t pos = 0;
    int depth = 200;
    for (int i = 0; i < depth; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "(if #t ");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "42");
    for (int i = 0; i < depth; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " 0)");

    mobj v = eval_str(buf);
    CHECK(Mfixnum_val(v) == 42, "deep if: nested chain reaches 42");
    Mshutdown();
}

int main(void) {
    test_self_evaluating();
    test_quote();
    test_if();
    test_begin();
    test_nested();
    test_deep_begin();
    test_deep_if();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
