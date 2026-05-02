#include "minim.h"
#include "gc.h"
#include "harness.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Phase 1 — heap shapes for the evaluator
 *
 * Constructs each new typed-object kind, forces a collection, then
 * checks that every slot survived. Run under MINIM_GC_STRESS for the
 * strongest signal that the GC's generic typed-object trace path is
 * correct for every secondary tag.
 * -------------------------------------------------------------------- */

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

    CHECK(Mclosurep(clo), "closure: predicate");
    CHECK(!Mvectorp(clo), "closure: not a vector");
    CHECK(!Menvp(clo),    "closure: not an env");

    gc_collect(0);

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

    CHECK(Menvp(env), "env: predicate");

    gc_collect(0);

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

    CHECK(Mprimp(p), "prim: predicate");
    CHECK(Mprocedurep(p), "prim: is a procedure");

    gc_collect(0);

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

    CHECK(Mcontp(cont), "cont: predicate");
    CHECK(Mprocedurep(cont), "cont: is a procedure");

    gc_collect(0);

    mobj inner = Mcont_kont(cont);
    CHECK(Mkontp(inner), "cont: wrapped kont survived");
    CHECK(Mfixnum_val(Mkont_kind(inner)) == 1, "cont: wrapped kind preserved");
    CHECK(Mkontp(Mkont_parent(inner)), "cont: parent chain survived");
    CHECK(Mfixnum_val(Mkont_kind(Mkont_parent(inner))) == 0,
          "cont: bottom of chain still halt");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

/* Sanity: every new typed object is distinguished from every other
 * kind by its predicate. */
static void test_kinds_disjoint(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj clo = Mnull, env = Mnull, k = Mnull, p = Mnull, c = Mnull, v = Mnull;
    mobj rib = Mnull;
    MINIM_GC_PROTECT(clo);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(k);
    MINIM_GC_PROTECT(p);
    MINIM_GC_PROTECT(c);
    MINIM_GC_PROTECT(v);
    MINIM_GC_PROTECT(rib);

    clo = Mclosure(Mnull, Mnull, Mnull, Mfalse);
    rib = Mvector(0, Mfalse);
    env = Menv_extend(rib, Mnull);
    k = Mkont(KONT_HALT, Mnull, Mnull, 0);
    p = Mprim("p", prim_dummy_add, 0, -1);
    c = Mcont(k);
    v = Mvector(2, Mfalse);

    /* Each kind matches only its own predicate among the typed-object
     * predicates. */
    CHECK( Mclosurep(clo) && !Menvp(clo) && !Mkontp(clo) && !Mprimp(clo) && !Mcontp(clo) && !Mvectorp(clo), "kinds: closure");
    CHECK(!Mclosurep(env) &&  Menvp(env) && !Mkontp(env) && !Mprimp(env) && !Mcontp(env) && !Mvectorp(env), "kinds: env");
    CHECK(!Mclosurep(k)   && !Menvp(k)   &&  Mkontp(k)   && !Mprimp(k)   && !Mcontp(k)   && !Mvectorp(k),   "kinds: kont");
    CHECK(!Mclosurep(p)   && !Menvp(p)   && !Mkontp(p)   &&  Mprimp(p)   && !Mcontp(p)   && !Mvectorp(p),   "kinds: prim");
    CHECK(!Mclosurep(c)   && !Menvp(c)   && !Mkontp(c)   && !Mprimp(c)   &&  Mcontp(c)   && !Mvectorp(c),   "kinds: cont");
    CHECK(!Mclosurep(v)   && !Menvp(v)   && !Mkontp(v)   && !Mprimp(v)   && !Mcontp(v)   &&  Mvectorp(v),   "kinds: vector");

    /* Procedures: closure, prim, cont. Not env, not kont, not vector. */
    CHECK(Mprocedurep(clo), "proc: closure is a procedure");
    CHECK(Mprocedurep(p),   "proc: prim is a procedure");
    CHECK(Mprocedurep(c),   "proc: cont is a procedure");
    CHECK(!Mprocedurep(env), "proc: env is not a procedure");
    CHECK(!Mprocedurep(k),   "proc: kont is not a procedure");
    CHECK(!Mprocedurep(v),   "proc: vector is not a procedure");
    CHECK(!Mprocedurep(Mfixnum(0)), "proc: fixnum is not a procedure");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

int main(void) {
    test_closure_survives_gc();
    test_env_survives_gc();
    test_kont_survives_gc();
    test_prim_survives_gc();
    test_cont_survives_gc();
    test_kinds_disjoint();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
