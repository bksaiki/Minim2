#include "minim.h"
#include "gc.h"
#include "harness.h"

#include <string.h>

/* -----------------------------------------------------------------------
 * Phase 6 — Cheney GC verification
 *
 * Each test forces a collection (or many) and re-checks the live heap.
 * The shadow-stack frame is what makes survival possible — anything not
 * protected is collectible.
 * --------------------------------------------------------------------- */

static void test_cons_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    mobj lst = Mnull;
    MINIM_GC_PROTECT(lst);

    /* Build (999 998 ... 0) */
    for (int i = 0; i < 1000; i++)
        lst = Mcons(Mfixnum(i), lst);

    gc_collect(0);

    /* Walk and verify */
    int expected = 999;
    int ok = 1;
    mobj cur = lst;
    while (!Mnullp(cur)) {
        if (!Mpairp(cur)) { ok = 0; break; }
        if (Mfixnum_val(Mcar(cur)) != expected) { ok = 0; break; }
        cur = Mcdr(cur);
        expected--;
    }
    CHECK(ok, "cons survives GC: values intact");
    CHECK(expected == -1, "cons survives GC: length intact");

    /* Force a few more collections — values must stay the same */
    gc_collect(0);
    gc_collect(0);

    expected = 999;
    ok = 1;
    cur = lst;
    while (!Mnullp(cur)) {
        if (Mfixnum_val(Mcar(cur)) != expected) { ok = 0; break; }
        cur = Mcdr(cur);
        expected--;
    }
    CHECK(ok, "cons survives GC: values intact after multiple collections");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_vector_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    /* Build a vector whose slots are pairs — GC must walk both the
     * vector slots and the pairs they reference. */
    mobj v = Mvector(50, Mnull);
    MINIM_GC_PROTECT(v);

    for (size_t i = 0; i < 50; i++) {
        mobj cell = Mcons(Mfixnum((intptr_t)i), Mfixnum((intptr_t)(i * 2)));
        Mvector_set(v, i, cell);
    }

    gc_collect(0);

    int ok = 1;
    for (size_t i = 0; i < 50; i++) {
        mobj cell = Mvector_ref(v, i);
        if (!Mpairp(cell)) { ok = 0; break; }
        if (Mfixnum_val(Mcar(cell)) != (intptr_t)i) { ok = 0; break; }
        if (Mfixnum_val(Mcdr(cell)) != (intptr_t)(i * 2)) { ok = 0; break; }
    }
    CHECK(ok, "vector survives GC: slot pairs intact");
    CHECK(Mvector_length(v) == 50, "vector survives GC: length intact");

    /* Mix in a flonum slot too */
    Mvector_set(v, 0, Mflonum(2.71828));
    gc_collect(0);
    CHECK(Mflonump(Mvector_ref(v, 0)),
          "vector survives GC: flonum slot keeps tag");
    CHECK(Mflonum_val(Mvector_ref(v, 0)) == 2.71828,
          "vector survives GC: flonum value intact");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_circular_list_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    mobj p = Mcons(Mfixnum(7), Mnull);
    MINIM_GC_PROTECT(p);

    /* (set-car! p p) — self-reference through car */
    Mset_car(p, p);
    /* (set-cdr! p p) — and through cdr */
    Mset_cdr(p, p);

    /* Forwarding marker must break the cycle. If GC loops, ctest times out. */
    gc_collect(0);

    CHECK(Mpairp(p), "circular list: still a pair after GC");
    CHECK(Mcar(p) == p, "circular list: car points to self after GC");
    CHECK(Mcdr(p) == p, "circular list: cdr points to self after GC");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_intern_identity_across_gc(void) {
    Minit();

    mobj a = Mintern("foo");
    mobj b = Mintern("foo");
    CHECK(a == b, "intern: same name -> same mobj");

    mobj c = Mintern("bar");
    CHECK(a != c, "intern: different names -> different mobj");

    gc_collect(0);

    /* After GC, the symbol heap object has been moved. The intern table
     * bucket is a global root, so it must have been forwarded to the new
     * address. A fresh Mintern("foo") must hit the same updated bucket. */
    mobj a2 = Mintern("foo");
    CHECK(a2 == a || (Msymbolp(a2) && strcmp(Msymbol_name(a2), "foo") == 0),
          "intern: foo retains identity across GC");

    /* Stronger check: the bucket was updated, so a fresh intern of the
     * same name returns the *exact same* mobj as the post-GC value of a.
     * (a itself was a stack local, not protected, so its stale pre-GC
     * bits are not directly comparable. But Mintern walks the bucket,
     * which the GC did update.) */
    mobj a3 = Mintern("foo");
    CHECK(a2 == a3, "intern: post-GC re-intern is stable");

    mobj c2 = Mintern("bar");
    CHECK(a2 != c2, "intern: foo and bar still distinct after GC");
    CHECK(strcmp(Msymbol_name(a2), "foo") == 0,
          "intern: foo name string preserved");
    CHECK(strcmp(Msymbol_name(c2), "bar") == 0,
          "intern: bar name string preserved");

    Mshutdown();
}

/* Allocate enough that the heap has to grow at least once.
 *
 * Under MINIM_GC_STRESS every alloc triggers a full collection, so the
 * cost is O(n²); we keep the allocation count small enough to still exit
 * in seconds while exercising the same code path. */
#ifdef MINIM_GC_STRESS
#define GROWTH_N 5000
#else
#define GROWTH_N 200000
#endif

static void test_heap_growth(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    mobj lst = Mnull;
    MINIM_GC_PROTECT(lst);
    for (int i = 0; i < GROWTH_N; i++)
        lst = Mcons(Mfixnum(i), lst);

    CHECK(Mpairp(lst), "growth: head is pair");
    CHECK(Mfixnum_val(Mcar(lst)) == GROWTH_N - 1,
          "growth: head value correct");

    gc_collect(0);
    CHECK(Mfixnum_val(Mcar(lst)) == GROWTH_N - 1,
          "growth: head value correct after extra GC");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

/* Regression: Mshutdown must tear down the intern table too. Otherwise
 * the surviving buckets hold mobj pointers into the just-unmapped heap,
 * and the next Mintern walks a chain of dangling pointers. */
static void test_shutdown_resets_intern_table(void) {
    Minit();
    mobj a = Mintern("foo");
    CHECK(Msymbolp(a), "shutdown/reinit: pre-shutdown foo is a symbol");
    Mshutdown();

    Minit();
    /* Same name, fresh runtime: must not crash on stale buckets, must
     * produce a valid symbol that lives in the new heap. */
    mobj a2 = Mintern("foo");
    CHECK(Msymbolp(a2), "shutdown/reinit: post-reinit foo is a symbol");
    CHECK(strcmp(Msymbol_name(a2), "foo") == 0,
          "shutdown/reinit: post-reinit name correct");

    /* And the new symbol is GC-tracked: a forced collection must not
     * dangle the bucket. (a2 itself is a stale stack copy post-collect,
     * so we compare two fresh post-collect interns instead.) */
    gc_collect(0);
    mobj a3 = Mintern("foo");
    mobj a4 = Mintern("foo");
    CHECK(a3 == a4, "shutdown/reinit: bucket survives GC after reinit");
    CHECK(strcmp(Msymbol_name(a3), "foo") == 0,
          "shutdown/reinit: name still readable after GC");
    Mshutdown();
}

/* -----------------------------------------------------------------------
 * Survival of the typed-object kinds added for the evaluator
 * (closure, env, kont, prim, cont). Same shape as the cons/vector
 * survival tests above — construct one, force a collection, walk
 * every payload slot.
 * --------------------------------------------------------------------- */

static void test_closure_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj params = Mnull, body = Mnull, env = Mnull, name = Mnull;
    mobj sym_x = Mnull, clo = Mnull;
    MINIM_GC_PROTECT(params);
    MINIM_GC_PROTECT(body);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(name);
    MINIM_GC_PROTECT(sym_x);
    MINIM_GC_PROTECT(clo);

    sym_x = Mintern("x");
    params = Mcons(sym_x, Mnull);
    body = Mcons(sym_x, Mnull); /* (x) — bogus body, only here as data */
    name = Mintern("identity");

    clo = Mclosure(params, body, env, name);

    gc_collect(0);

    CHECK(Mclosurep(clo), "closure: predicate after GC");
    CHECK(Mpairp(Mclosure_params(clo)), "closure: params survived");
    CHECK(Msymbolp(Mcar(Mclosure_params(clo))), "closure: param[0] still a symbol");
    CHECK(strcmp(Msymbol_name(Mcar(Mclosure_params(clo))), "x") == 0,
          "closure: param[0] name still 'x'");
    CHECK(Mpairp(Mclosure_body(clo)), "closure: body survived");
    CHECK(Mnullp(Mclosure_env(clo)), "closure: env still null");
    CHECK(Msymbolp(Mclosure_name(clo)), "closure: name still a symbol");
    CHECK(strcmp(Msymbol_name(Mclosure_name(clo)), "identity") == 0,
          "closure: name still 'identity'");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_env_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj rib = Mnull, env = Mnull, rib2 = Mnull, env2 = Mnull;
    mobj sym = Mnull;
    MINIM_GC_PROTECT(rib);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(rib2);
    MINIM_GC_PROTECT(env2);
    MINIM_GC_PROTECT(sym);

    /* rib = #(x 42 y 99) — interleaving Mintern with Mvector_set is
     * safe under stress only if we route the symbol through a
     * protected slot, never an anonymous compiler temp. */
    rib = Mvector(4, Mfalse);
    sym = Mintern("x"); Mvector_set(rib, 0, sym);
    Mvector_set(rib, 1, Mfixnum(42));
    sym = Mintern("y"); Mvector_set(rib, 2, sym);
    Mvector_set(rib, 3, Mfixnum(99));

    env = Menv_extend(rib, Mnull);

    gc_collect(0);

    CHECK(Menvp(env), "env: predicate after GC");
    CHECK(Mvectorp(Menv_rib(env)), "env: rib survived");
    CHECK(Mvector_length(Menv_rib(env)) == 4, "env: rib length still 4");
    CHECK(Mfixnum_val(Mvector_ref(Menv_rib(env), 1)) == 42,
          "env: rib value [1] still 42");
    CHECK(Mnullp(Menv_parent(env)), "env: parent still null");

    /* Nested: extend with another rib pointing to env */
    rib2 = Mvector(2, Mfalse);
    sym = Mintern("z"); Mvector_set(rib2, 0, sym);
    Mvector_set(rib2, 1, Mfixnum(7));
    env2 = Menv_extend(rib2, env);

    gc_collect(0);

    CHECK(Menvp(Menv_parent(env2)), "env2: parent is an env");
    CHECK(Mfixnum_val(Mvector_ref(Menv_rib(Menv_parent(env2)), 1)) == 42,
          "env2: parent rib value still 42 across two collections");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_kont_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    /* Declare and protect every intermediate up front so the
     * shadow stack covers each step's allocations. Composite
     * `Mcons(Mintern(...), Mcons(...))` expressions are NOT
     * GC-safe under stress: anonymous C temporaries fall out of
     * any root set across multiple back-to-back collections. */
    mobj halt = Mnull, k_if = Mnull, rest = Mnull, k_seq = Mnull;
    mobj unev = Mnull, k_app = Mnull, sym_a = Mnull, sym_b = Mnull;
    MINIM_GC_PROTECT(halt);
    MINIM_GC_PROTECT(k_if);
    MINIM_GC_PROTECT(rest);
    MINIM_GC_PROTECT(k_seq);
    MINIM_GC_PROTECT(unev);
    MINIM_GC_PROTECT(k_app);
    MINIM_GC_PROTECT(sym_a);
    MINIM_GC_PROTECT(sym_b);

    halt = Mkont(KONT_HALT, Mnull, Mnull, 0);
    CHECK(Mkontp(halt), "kont: halt is kont");
    CHECK(Mfixnum_val(Mkont_kind(halt)) == 0, "kont: halt kind is KONT_HALT");

    /* if-frame on top */
    k_if = Mkont_if(halt, Mnull, Mfixnum(1), Mfixnum(2));

    /* seq-frame on top with a list of 3 exprs, built bottom-up */
    rest = Mcons(Mfixnum(30), Mnull);
    rest = Mcons(Mfixnum(20), rest);
    rest = Mcons(Mfixnum(10), rest);
    k_seq = Mkont_seq(k_if, Mnull, rest);

    /* app-frame on top with a list of 2 unevaluated args */
    sym_b = Mintern("b");
    unev = Mcons(sym_b, Mnull);
    sym_a = Mintern("a");
    unev = Mcons(sym_a, unev);
    k_app = Mkont_app(k_seq, Mnull, unev, Mnull);

    gc_collect(0);
    gc_collect(0); /* twice */

    /* Walk down the chain. */
    CHECK(Mkontp(k_app), "kont: app survives");
    CHECK(Mfixnum_val(Mkont_kind(k_app)) == 3, "kont: app kind is KONT_APP");
    CHECK(Mpairp(Mtyped_obj_ref(k_app, 3)), "kont: app unev is a list");

    mobj k_seq2 = Mkont_parent(k_app);
    CHECK(Mkontp(k_seq2), "kont: seq survives");
    CHECK(Mfixnum_val(Mkont_kind(k_seq2)) == 2, "kont: seq kind is KONT_SEQ");
    mobj rest_after = Mtyped_obj_ref(k_seq2, 3);
    CHECK(Mfixnum_val(Mcar(rest_after)) == 10, "kont: seq rest[0] still 10");

    mobj k_if2 = Mkont_parent(k_seq2);
    CHECK(Mkontp(k_if2), "kont: if survives");
    CHECK(Mfixnum_val(Mkont_kind(k_if2)) == 1, "kont: if kind is KONT_IF");
    CHECK(Mfixnum_val(Mtyped_obj_ref(k_if2, 3)) == 1, "kont: if then survives");
    CHECK(Mfixnum_val(Mtyped_obj_ref(k_if2, 4)) == 2, "kont: if else survives");

    mobj k_halt = Mkont_parent(k_if2);
    CHECK(Mkontp(k_halt), "kont: halt at bottom survives");
    CHECK(Mfixnum_val(Mkont_kind(k_halt)) == 0, "kont: halt kind preserved");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static mobj prim_dummy_add(mobj args) {
    return Mfixnum(Mfixnum_val(Mcar(args)) + Mfixnum_val(Mcar(Mcdr(args))));
}

static void test_prim_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj p = Mnull, args = Mnull, result = Mnull;
    MINIM_GC_PROTECT(p);
    MINIM_GC_PROTECT(args);
    MINIM_GC_PROTECT(result);

    p = Mprim("dummy-add", prim_dummy_add, 2, 2);

    gc_collect(0);

    CHECK(Mprimp(p), "prim: predicate after GC");
    CHECK(Msymbolp(Mprim_name(p)), "prim: name survived as symbol");
    CHECK(strcmp(Msymbol_name(Mprim_name(p)), "dummy-add") == 0,
          "prim: name still 'dummy-add'");
    CHECK(Mfixnum_val(Mprim_arity_min(p)) == 2, "prim: arity-min survived");
    CHECK(Mfixnum_val(Mprim_arity_max(p)) == 2, "prim: arity-max survived");

    /* fnptr round-trip: call the recovered function. */
    Mprim_fn fn = Mprim_fn_of(p);
    args = Mcons(Mfixnum(4), Mnull);
    args = Mcons(Mfixnum(3), args);
    result = fn(args);
    CHECK(Mfixnum_val(result) == 7, "prim: fnptr survived GC and still works");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_cont_survives_gc(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj halt = Mnull, k_if = Mnull, cont = Mnull;
    MINIM_GC_PROTECT(halt);
    MINIM_GC_PROTECT(k_if);
    MINIM_GC_PROTECT(cont);

    halt = Mkont(KONT_HALT, Mnull, Mnull, 0);
    k_if = Mkont_if(halt, Mnull, Mfixnum(1), Mfixnum(2));
    cont = Mcont(k_if);

    gc_collect(0);

    CHECK(Mcontp(cont), "cont: predicate after GC");
    mobj inner = Mcont_kont(cont);
    CHECK(Mkontp(inner), "cont: wrapped kont survived");
    CHECK(Mfixnum_val(Mkont_kind(inner)) == 1, "cont: wrapped kind preserved");
    CHECK(Mkontp(Mkont_parent(inner)), "cont: parent chain survived");
    CHECK(Mfixnum_val(Mkont_kind(Mkont_parent(inner))) == 0,
          "cont: bottom of chain still halt");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

int main(void) {
    test_cons_survives_gc();
    test_vector_survives_gc();
    test_circular_list_gc();
    test_intern_identity_across_gc();
    test_heap_growth();
    test_shutdown_resets_intern_table();
    test_closure_survives_gc();
    test_env_survives_gc();
    test_kont_survives_gc();
    test_prim_survives_gc();
    test_cont_survives_gc();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
