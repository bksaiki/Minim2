#include "minim.h"
#include "harness.h"

#include <setjmp.h>
#include <string.h>

/* ----------------------------------------------------------------------
 * Phase 2 — `import` special form
 *
 * v1 supports only `(import (prefix module-ref prefix-sym))`. Tests
 * verify the happy path against `#%kernel`, plus the error paths
 * (body-position, unknown module, malformed spec, non-symbol parts).
 * Recoverable errors are caught with the same setjmp pattern used
 * elsewhere — see test_eval.c for the canonical example.
 * -------------------------------------------------------------------- */

static mobj eval_str(const char *src) {
    mreader r;
    mreader_init_string(&r, src);
    mobj expr = Mread(&r);
    return Meval(expr);
}

static mobj eval_seq(const char *src) {
    MINIM_GC_FRAME_BEGIN;
    mobj last = Mvoid;
    MINIM_GC_PROTECT(last);
    mreader r;
    mreader_init_string(&r, src);
    for (;;) {
        mobj expr = Mread(&r);
        if (Meofp(expr)) break;
        last = Meval(expr);
    }
    MINIM_GC_RETURN(last);
}

/* Run `src`, expecting Merror to fire. Returns true if it did. */
static int eval_caught_error(const char *src) {
    jmp_buf jmp;
    minim_error_jmp = &jmp;
    minim_error_jmp_ssp = minim_ssp;
    int errored = 0;
    if (setjmp(jmp) == 0) eval_str(src); else errored = 1;
    minim_error_jmp = NULL;
    return errored;
}

static void test_prefix_import_basic(void) {
    Minit();
    /* Prefix import installs the entire kernel under the given
     * prefix in the top-level env. With dual-population (Phase 1),
     * the canonical names also exist; the prefixed names are
     * additional. */
    eval_str("(import (prefix #%kernel $))");

    /* Identity: $car/$cdr/$cons walk a list. */
    CHECK(Mfixnum_val(eval_str("($car '(1 2 3))")) == 1,
          "import: $car works");
    CHECK(Mfixnum_val(eval_str("($car ($cdr '(1 2 3)))")) == 2,
          "import: $car ($cdr ...) works");
    mobj v = eval_str("($cons 7 8)");
    CHECK(Mpairp(v) && Mfixnum_val(Mcar(v)) == 7 && Mfixnum_val(Mcdr(v)) == 8,
          "import: $cons builds a pair");

    /* Arithmetic primitives are also imported. */
    CHECK(Mfixnum_val(eval_str("($+ 1 2 3)")) == 6,    "import: $+ works");
    CHECK(Mfixnum_val(eval_str("($* 2 3 4)")) == 24,   "import: $* works");
    CHECK(Mtruep(eval_str("($= 1 1)")),                 "import: $= works");

    /* Prefixed and canonical refer to the same primitive object —
     * eq? holds. (Both top-level entries point at the same Mprim
     * heap object.) */
    CHECK(Mtruep(eval_str("(eq? car $car)")),
          "import: $car eq? car (same prim object)");

    /* Predicates work too. */
    CHECK(Mtruep(eval_str("($pair? '(1 2))")),          "import: $pair?");
    CHECK(Mfalsep(eval_str("($null? 'foo)")),           "import: $null?");
    Mshutdown();
}

static void test_prefix_import_alt_prefix(void) {
    Minit();
    /* Any symbol can serve as the prefix — the prefix is concatenated
     * verbatim. */
    eval_str("(import (prefix #%kernel k:))");
    CHECK(Mfixnum_val(eval_str("(k:car '(1 2))")) == 1, "import: k: prefix");
    CHECK(Mfixnum_val(eval_str("(k:+ 10 20)")) == 30,    "import: k:+ works");

    /* Empty-ish prefix: a single character `_` works just fine. */
    Mshutdown();

    Minit();
    eval_str("(import (prefix #%kernel _))");
    CHECK(Mfixnum_val(eval_str("(_car '(9))")) == 9, "import: _ prefix");
    Mshutdown();
}

static void test_prefix_import_idempotent(void) {
    Minit();
    /* Re-importing the same prefix should not error and should leave
     * the bindings intact (top_env_define replaces in place). */
    eval_seq(
        "(import (prefix #%kernel $))"
        "(import (prefix #%kernel $))");
    CHECK(Mfixnum_val(eval_str("($+ 1 2)")) == 3,
          "import: re-import is idempotent");
    Mshutdown();
}

static void test_prefix_import_body_rejected(void) {
    Minit();
    /* Body-position import is rejected — like internal define. */
    CHECK(eval_caught_error(
              "(let ((x 1)) (import (prefix #%kernel $)) x)"),
          "import: rejected inside let body");
    CHECK(eval_caught_error(
              "((lambda () (import (prefix #%kernel $))))"),
          "import: rejected inside lambda body");
    Mshutdown();
}

static void test_bare_import(void) {
    Minit();
    /* (import M) installs every binding under its original name.
     * With dual-population (Phase 1) the canonical names already
     * exist, so this re-binds them in place — exercising the
     * top_env_define replace-in-place path. */
    eval_str("(import #%kernel)");

    CHECK(Mfixnum_val(eval_str("(car '(1 2 3))")) == 1,
          "bare import: car works");
    CHECK(Mfixnum_val(eval_str("(+ 1 2 3)")) == 6,
          "bare import: + works");
    CHECK(Mtruep(eval_str("(pair? '(1))")),
          "bare import: pair? works");
    Mshutdown();
}

static void test_multi_spec(void) {
    Minit();
    /* (import s1 s2 ...) iterates left-to-right. The bare import
     * lands the canonical names; the prefixed import then layers
     * the `$` aliases on top. Both end up bound to the same prim. */
    eval_str("(import #%kernel (prefix #%kernel $))");
    CHECK(Mfixnum_val(eval_str("(car '(7 8))")) == 7,    "multi-spec: car");
    CHECK(Mfixnum_val(eval_str("($car '(7 8))")) == 7,   "multi-spec: $car");
    CHECK(Mtruep(eval_str("(eq? car $car)")),
          "multi-spec: aliases share identity");
    Mshutdown();
}

static void test_import_unknown_module(void) {
    Minit();
    /* Prefixed form. */
    CHECK(eval_caught_error("(import (prefix #%does-not-exist $))"),
          "import: unknown module errors (prefixed)");
    CHECK(eval_caught_error("(import (prefix kernel $))"),
          "import: bare 'kernel' is not the kernel module");

    /* Bare form. */
    CHECK(eval_caught_error("(import #%does-not-exist)"),
          "import: unknown module errors (bare)");
    CHECK(eval_caught_error("(import kernel)"),
          "import: bare 'kernel' rejected as unknown");
    Mshutdown();
}

static void test_prefix_import_malformed(void) {
    Minit();
    /* Zero specs. */
    CHECK(eval_caught_error("(import)"),
          "import: zero specs errors");

    /* Spec is neither a symbol nor a (prefix ...) list. */
    CHECK(eval_caught_error("(import 1)"),
          "import: spec is fixnum");
    CHECK(eval_caught_error("(import 'foo)"),
          "import: spec is (quote foo) — pair but not prefix");
    CHECK(eval_caught_error("(import (only #%kernel car))"),
          "import: only-form rejected (not yet supported)");

    /* Prefix-shaped but wrong length. */
    CHECK(eval_caught_error("(import (prefix #%kernel))"),
          "import: prefix spec too short");
    CHECK(eval_caught_error("(import (prefix #%kernel $ extra))"),
          "import: prefix spec too long");

    /* Module-ref and prefix must be symbols. */
    CHECK(eval_caught_error("(import (prefix 1 $))"),
          "import: module-ref must be symbol");
    CHECK(eval_caught_error("(import (prefix #%kernel 1))"),
          "import: prefix must be symbol");
    Mshutdown();
}

int main(void) {
    test_prefix_import_basic();
    test_prefix_import_alt_prefix();
    test_prefix_import_idempotent();
    test_prefix_import_body_rejected();
    test_bare_import();
    test_multi_spec();
    test_import_unknown_module();
    test_prefix_import_malformed();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
