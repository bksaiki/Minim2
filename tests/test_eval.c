#include "minim.h"
#include "harness.h"

#include <setjmp.h>
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

/* Read a stream of multiple top-level forms, eval each in sequence,
 * and return the value of the last. Used by Phase 5 tests where
 * `define`/`set!` need to mutate state across distinct top-level
 * expressions. The result slot is GC-protected because Mread
 * allocates between iterations. */
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

/* ----------------------------------------------------------------------
 * Phase 3 — environments and `let`
 * -------------------------------------------------------------------- */

static void test_let_basic(void) {
    Minit();
    /* Single binding. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) x)")) == 1,
          "let: (let ((x 1)) x) = 1");

    /* Multiple bindings, last wins as result via tail `begin`. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1) (y 2)) y)")) == 2,
          "let: (let ((x 1) (y 2)) y) = 2");
    CHECK(Mfixnum_val(eval_str("(let ((x 1) (y 2)) x)")) == 1,
          "let: (let ((x 1) (y 2)) x) = 1");

    /* Empty bindings. */
    CHECK(Mfixnum_val(eval_str("(let () 42)")) == 42, "let: empty bindings");

    /* Body is implicit begin; non-tail values discarded. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) 99 100 x)")) == 1,
          "let: body is implicit begin; tail value wins");

    /* Inits evaluated left-to-right in the *outer* env, not in the
     * let's own scope. (Phase 3 has no symbol the inner init could
     * reference, but the structural check still matters: y's init
     * doesn't see x.) Verified with constants. */
    CHECK(Mfixnum_val(eval_str("(let ((x 10) (y 20)) y)")) == 20,
          "let: parallel bindings, second wins as tail");
    Mshutdown();
}

static void test_let_inits(void) {
    Minit();
    /* Init expressions are themselves evaluated. */
    CHECK(Mfixnum_val(eval_str("(let ((x (if #t 7 8))) x)")) == 7,
          "let: init uses if");
    CHECK(Mfixnum_val(eval_str("(let ((x (begin 1 2 3))) x)")) == 3,
          "let: init uses begin");

    /* Quoted-list init. */
    mobj v = eval_str("(let ((x (quote (a b c)))) x)");
    CHECK(Mpairp(v) && Msymbolp(Mcar(v)) &&
          strcmp(Msymbol_name(Mcar(v)), "a") == 0,
          "let: init is a quoted list");
    Mshutdown();
}

static void test_let_nested(void) {
    Minit();
    /* Inner let's body sees outer binding via parent walk. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) (let ((y 2)) x))")) == 1,
          "let: inner sees outer x");
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) (let ((y 2)) y))")) == 2,
          "let: inner y resolves locally");

    /* Shadowing: inner x hides outer x. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) (let ((x 2)) x))")) == 2,
          "let: inner shadows outer");
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) (let ((x 2)) (let ((x 3)) x)))")) == 3,
          "let: 3-deep shadowing resolves to innermost");

    /* Body of an outer let after returning from inner: outer binding
     * is still visible. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 1)) (begin (let ((x 99)) x) x))")) == 1,
          "let: outer x intact after inner let returns");
    Mshutdown();
}

static void test_let_in_if(void) {
    Minit();
    /* let inside an if branch. */
    CHECK(Mfixnum_val(eval_str("(if #t (let ((x 5)) x) 99)")) == 5,
          "let: as true branch");
    CHECK(Mfixnum_val(eval_str("(if #f 99 (let ((x 6)) x))")) == 6,
          "let: as false branch");

    /* if inside a let body. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) (if #t x 99))")) == 1,
          "let: body uses bound var in if test result");
    Mshutdown();
}

static void test_let_unbound_error(void) {
    Minit();
    /* A let that doesn't bind `y` and references it should produce
     * the standard unbound-variable error. The REPL recovers via
     * Merror; here we install a setjmp handler manually so the test
     * can verify the path without aborting. */
    jmp_buf jmp;
    minim_error_jmp = &jmp;
    minim_error_jmp_ssp = minim_ssp;
    int errored = 0;
    if (setjmp(jmp) == 0) {
        eval_str("(let ((x 1)) y)");
    } else {
        errored = 1;
    }
    minim_error_jmp = NULL;
    CHECK(errored, "let: reference to unbound symbol triggers Merror");
    Mshutdown();
}

static void test_let_many(void) {
    Minit();
    /* Many bindings; the rib walk and KONT_LET advance each work
     * correctly. The tail is the very-last variable, so its value
     * propagates out. Stress mode exercises the protect frame in
     * step_apply's KONT_LET branch — every binding triggers a
     * Mcons + later Mvector + Menv_extend. */
    char buf[2048];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "(let (");
    for (int i = 0; i < 50; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " (v%d %d)", i, i);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ") v49)");
    mobj v = eval_str(buf);
    CHECK(Mfixnump(v) && Mfixnum_val(v) == 49,
          "let: 50-binding form, tail var resolves to its init");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Phase 4 — `lambda` and procedure application
 * -------------------------------------------------------------------- */

static void test_lambda_basic(void) {
    Minit();
    /* Identity. */
    CHECK(Mfixnum_val(eval_str("((lambda (x) x) 42)")) == 42,
          "lambda: identity");

    /* Two-arg, body returns second. */
    CHECK(Mfixnum_val(eval_str("((lambda (x y) y) 1 2)")) == 2,
          "lambda: two-arg, second wins");
    CHECK(Mfixnum_val(eval_str("((lambda (x y) x) 1 2)")) == 1,
          "lambda: two-arg, first");

    /* Zero-arg. */
    CHECK(Mfixnum_val(eval_str("((lambda () 99))")) == 99,
          "lambda: zero-arg");

    /* Multi-expression body — implicit begin, last is tail. */
    CHECK(Mfixnum_val(eval_str("((lambda (x) 1 2 x) 7)")) == 7,
          "lambda: multi-expr body");

    /* Lambda evaluates to a closure (a procedure). */
    CHECK(Mprocedurep(eval_str("(lambda (x) x)")),
          "lambda: evaluates to a procedure");
    CHECK(Mclosurep(eval_str("(lambda (x) x)")),
          "lambda: evaluates to a closure specifically");
    Mshutdown();
}

static void test_closure_capture(void) {
    Minit();
    /* Closure remembers its captured value. */
    CHECK(Mfixnum_val(eval_str("(((lambda (x) (lambda () x)) 7))")) == 7,
          "closure: captured x");

    /* Two captures from the same outer constructor each retain
     * their own captured value — branch chooses which to invoke. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((make-const (lambda (n) (lambda () n))))"
        "  (let ((c1 (make-const 10)))"
        "    (let ((c2 (make-const 20)))"
        "      (if #t (c1) (c2)))))")) == 10,
        "closure: each captured n stays independent (c1 path)");
    CHECK(Mfixnum_val(eval_str(
        "(let ((make-const (lambda (n) (lambda () n))))"
        "  (let ((c1 (make-const 10)))"
        "    (let ((c2 (make-const 20)))"
        "      (if #f (c1) (c2)))))")) == 20,
        "closure: each captured n stays independent (c2 path)");

    /* Closure capturing through a let. */
    CHECK(Mfixnum_val(eval_str("(let ((x 5)) ((lambda () x)))")) == 5,
          "closure: captures let-bound var");

    /* Inner closure captures outer let; outer's binding survives the
     * inner lambda's invocation. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 11))"
        "  ((lambda () x)))")) == 11,
        "closure: captures outer let across lambda boundary");
    Mshutdown();
}

static void test_lambda_shadowing(void) {
    Minit();
    /* lambda's parameter shadows an outer binding. */
    CHECK(Mfixnum_val(eval_str("(let ((x 1)) ((lambda (x) x) 99))")) == 99,
          "shadow: lambda param hides let-bound var");

    /* After lambda returns, outer var is intact. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 1))"
        "  (begin"
        "    ((lambda (x) x) 99)"
        "    x))")) == 1,
        "shadow: outer x intact after lambda");

    /* Nested lambdas with same param name. */
    CHECK(Mfixnum_val(eval_str(
        "(((lambda (x) (lambda (x) x)) 1) 2)")) == 2,
        "shadow: inner lambda param hides outer");
    Mshutdown();
}

static void test_let_in_lambda(void) {
    Minit();
    CHECK(Mfixnum_val(eval_str(
        "((lambda (x) (let ((y 10)) y)) 99)")) == 10,
        "let-in-lambda: inner let result");

    CHECK(Mfixnum_val(eval_str(
        "((lambda (x) (let ((y 10)) x)) 99)")) == 99,
        "let-in-lambda: lambda param visible from let body");

    /* Let body containing a lambda that closes over the let binding. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 7))"
        "  (let ((f (lambda () x)))"
        "    (f)))")) == 7,
        "let-in-lambda: let-bound closure invoked");
    Mshutdown();
}

static void test_higher_order(void) {
    Minit();
    /* Pass a closure as an argument. */
    CHECK(Mfixnum_val(eval_str(
        "((lambda (f) (f)) (lambda () 42))")) == 42,
        "ho: closure as arg, then invoked");

    CHECK(Mfixnum_val(eval_str(
        "((lambda (f x) (f x)) (lambda (y) y) 5)")) == 5,
        "ho: apply received closure to received arg");

    /* Compose-ish: (apply f x) where f is identity. */
    CHECK(Mfixnum_val(eval_str(
        "(((lambda (f) (lambda (x) (f x))) (lambda (y) y)) 8)")) == 8,
        "ho: 2-level closure that wraps identity");
    Mshutdown();
}

/* Errors */

static int eval_caught_error(const char *src) {
    jmp_buf jmp;
    minim_error_jmp = &jmp;
    minim_error_jmp_ssp = minim_ssp;
    int errored = 0;
    if (setjmp(jmp) == 0) {
        eval_str(src);
    } else {
        errored = 1;
    }
    minim_error_jmp = NULL;
    return errored;
}

static void test_application_errors(void) {
    Minit();
    /* Arity mismatch — too few args. */
    CHECK(eval_caught_error("((lambda (x y) x) 1)"),
          "error: too few args");
    /* Arity mismatch — too many. */
    CHECK(eval_caught_error("((lambda (x) x) 1 2)"),
          "error: too many args");
    /* Application of non-procedure. */
    CHECK(eval_caught_error("(42 1 2)"),
          "error: applying a fixnum");
    CHECK(eval_caught_error("(#t)"),
          "error: applying #t");
    /* Malformed lambda. */
    CHECK(eval_caught_error("(lambda)"), "error: lambda with no args");
    CHECK(eval_caught_error("(lambda (1) 1)"),
          "error: lambda with non-symbol param");
    Mshutdown();
}

static void test_lambda_many_args(void) {
    Minit();
    /* 50-arg lambda; body returns the last param. Stresses both the
     * KONT_APP arg-walk and the rib build. */
    char buf[8192];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "((lambda (");
    for (int i = 0; i < 50; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "p%d ", i);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ") p49)");
    for (int i = 0; i < 50; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " %d", i);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ")");

    mobj v = eval_str(buf);
    CHECK(Mfixnum_val(v) == 49, "lambda: 50-arg call returns last param");
    Mshutdown();
}

/* ----------------------------------------------------------------------
 * Phase 5 — top-level env, `define`, `set!`
 * -------------------------------------------------------------------- */

static void test_define_basic(void) {
    Minit();
    /* define returns #<void>, then x looks up from top-level. */
    CHECK(Mvoidp(eval_str("(define x 1)")), "define: returns #<void>");
    CHECK(Mfixnum_val(eval_seq("(define x 1) x")) == 1,
          "define: top-level binding visible");

    /* Re-defining replaces. */
    CHECK(Mfixnum_val(eval_seq("(define x 1) (define x 2) x")) == 2,
          "define: redefining replaces");

    /* Top-level binding visible from inside let body. */
    CHECK(Mfixnum_val(eval_seq("(define g 99) (let ((y 1)) g)")) == 99,
          "define: top-level visible from inside let");

    /* Closure body sees top-level via env_lookup fallthrough. */
    CHECK(Mfixnum_val(eval_seq(
        "(define n 7)"
        "((lambda () n))")) == 7,
        "define: closure body resolves top-level");
    Mshutdown();
}

static void test_define_names_closure(void) {
    Minit();
    /* (define f (lambda ...)) fills the closure's name slot. */
    mobj v = eval_seq("(define id (lambda (x) x)) id");
    CHECK(Mclosurep(v), "define-name: result is a closure");
    CHECK(Msymbolp(Mclosure_name(v)), "define-name: name is a symbol");
    CHECK(strcmp(Msymbol_name(Mclosure_name(v)), "id") == 0,
          "define-name: closure name = 'id'");

    /* If the closure already has a name (e.g., aliased via another
     * define), the *original* name is preserved. */
    v = eval_seq("(define f (lambda (x) x)) (define g f) g");
    CHECK(Mclosurep(v), "define-alias: still a closure");
    CHECK(strcmp(Msymbol_name(Mclosure_name(v)), "f") == 0,
          "define-alias: keeps original name 'f'");
    Mshutdown();
}

static void test_set_lexical(void) {
    Minit();
    /* set! on a let-bound variable. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 1)) (begin (set! x 99) x))")) == 99,
        "set!: lexical let-bound mutation");

    /* set! on a lambda parameter. */
    CHECK(Mfixnum_val(eval_str(
        "((lambda (x) (begin (set! x 99) x)) 1)")) == 99,
        "set!: lambda-param mutation");

    /* set! returns #<void>. */
    CHECK(Mvoidp(eval_str("(let ((x 1)) (set! x 2))")),
          "set!: returns #<void>");

    /* Inner set! doesn't affect outer same-named binding. */
    CHECK(Mfixnum_val(eval_str(
        "(let ((x 1))"
        "  (begin"
        "    (let ((x 100)) (set! x 999))"
        "    x))")) == 1,
        "set!: inner shadow scope only");
    Mshutdown();
}

static void test_set_top_level(void) {
    Minit();
    CHECK(Mfixnum_val(eval_seq("(define x 1) (set! x 99) x")) == 99,
          "set!: top-level mutation");

    /* set! falls through to top-level when no lexical match. */
    CHECK(Mfixnum_val(eval_seq(
        "(define g 0)"
        "((lambda () (set! g 42)))"
        "g")) == 42,
        "set!: lambda mutates top-level via fallthrough");
    Mshutdown();
}

static void test_set_unbound_error(void) {
    Minit();
    jmp_buf jmp;
    minim_error_jmp = &jmp;
    minim_error_jmp_ssp = minim_ssp;
    int errored = 0;
    if (setjmp(jmp) == 0) {
        eval_str("(set! nonexistent 42)");
    } else {
        errored = 1;
    }
    minim_error_jmp = NULL;
    CHECK(errored, "set!: unbound variable triggers Merror");
    Mshutdown();
}

static void test_closure_state(void) {
    Minit();
    /* Counter pattern via captured set! — no `+` primitive needed
     * because we just toggle a boolean. The closure carries a
     * mutable `flag` cell across invocations. */
    eval_seq(
        "(define toggle"
        "  (let ((flag #f))"
        "    (lambda ()"
        "      (begin"
        "        (set! flag (if flag #f #t))"
        "        flag))))");

    CHECK(Mtruep(eval_str("(toggle)")),  "stateful closure: 1st → #t");
    CHECK(Mfalsep(eval_str("(toggle)")), "stateful closure: 2nd → #f");
    CHECK(Mtruep(eval_str("(toggle)")),  "stateful closure: 3rd → #t");

    /* Two separate toggles have independent state. */
    eval_seq(
        "(define make-toggle"
        "  (lambda ()"
        "    (let ((flag #f))"
        "      (lambda ()"
        "        (begin"
        "          (set! flag (if flag #f #t))"
        "          flag)))))");
    eval_seq("(define t1 (make-toggle)) (define t2 (make-toggle))");

    CHECK(Mtruep(eval_str("(t1)")),  "two toggles: t1 1st → #t");
    CHECK(Mtruep(eval_str("(t2)")),  "two toggles: t2 1st → #t (independent state)");
    CHECK(Mfalsep(eval_str("(t1)")), "two toggles: t1 2nd → #f");
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
    test_let_basic();
    test_let_inits();
    test_let_nested();
    test_let_in_if();
    test_let_unbound_error();
    test_let_many();
    test_lambda_basic();
    test_closure_capture();
    test_lambda_shadowing();
    test_let_in_lambda();
    test_higher_order();
    test_application_errors();
    test_lambda_many_args();
    test_define_basic();
    test_define_names_closure();
    test_set_lexical();
    test_set_top_level();
    test_set_unbound_error();
    test_closure_state();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
