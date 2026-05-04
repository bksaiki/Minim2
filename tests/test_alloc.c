#include "minim.h"
#include "harness.h"

/* -----------------------------------------------------------------------
 * Phase 2 — bump allocator smoke tests (no GC triggered)
 * --------------------------------------------------------------------- */

static void test_cons_list_1000(void) {
    Minit();

    /* Build a list: (999 998 ... 1 0) */
    mobj lst = Mnull;
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(lst);
    for (int i = 0; i < 1000; i++)
        lst = Mcons(Mfixnum(i), lst);

    /* Walk and verify values descend from 999 to 0 */
    int expected = 999;
    int ok = 1;
    mobj cur = lst;
    while (!Mnullp(cur)) {
        CHECK(Mpairp(cur), "cons list: element is pair");
        if (Mfixnum_val(Mcar(cur)) != expected)
            ok = 0;
        cur = Mcdr(cur);
        expected--;
    }
    CHECK(ok, "cons list: all 1000 values correct");
    CHECK(expected == -1, "cons list: exactly 1000 elements");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_flonum_heap_roundtrip(void) {
    Minit();
    mobj v = Mflonum(3.14159);
    CHECK(Mflonump(v), "flonum: tag");
    CHECK(Mflonum_val(v) == 3.14159, "flonum: value round-trip");
    Mshutdown();
}

static void test_vector_smoke(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    mobj v = Mvector(10, Mfixnum(0));
    MINIM_GC_PROTECT(v);

    CHECK(Mvectorp(v), "vector: vectorp");
    CHECK(Mvector_length(v) == 10, "vector: length 10");
    for (size_t i = 0; i < 10; i++)
        CHECK(Mfixnum_val(Mvector_ref(v, i)) == 0, "vector: fill is 0");

    Mvector_set(v, 5, Mfixnum(42));
    CHECK(Mfixnum_val(Mvector_ref(v, 5)) == 42, "vector: set/ref round-trip");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

/* -----------------------------------------------------------------------
 * Predicate disjointness across the typed-object kinds. Allocates one
 * of each, exercises every predicate, and confirms that exactly one
 * fires per kind.
 * --------------------------------------------------------------------- */

static mobj prim_dummy(mobj args) { (void)args; return Mfalse; }

static void test_kinds_disjoint(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;
    mobj clo = Mnull, env = Mnull, k = Mnull, p = Mnull, v = Mnull;
    mobj rib = Mnull;
    MINIM_GC_PROTECT(clo);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(k);
    MINIM_GC_PROTECT(p);
    MINIM_GC_PROTECT(v);
    MINIM_GC_PROTECT(rib);

    clo = Mclosure(Mnull, Mnull, Mnull, Mfalse);
    rib = Mvector(0, Mfalse);
    env = Menv_extend(rib, Mnull);
    k = Mkont(KONT_HALT, Mnull, Mnull, 0);
    p = Mprim("p", prim_dummy, 0, -1);
    v = Mvector(2, Mfalse);

    /* Each kind matches only its own predicate among the typed-object
     * predicates. */
    CHECK( Mclosurep(clo) && !Menvp(clo) && !Mkontp(clo) && !Mprimp(clo) && !Mvectorp(clo), "kinds: closure");
    CHECK(!Mclosurep(env) &&  Menvp(env) && !Mkontp(env) && !Mprimp(env) && !Mvectorp(env), "kinds: env");
    CHECK(!Mclosurep(k)   && !Menvp(k)   &&  Mkontp(k)   && !Mprimp(k)   && !Mvectorp(k),   "kinds: kont");
    CHECK(!Mclosurep(p)   && !Menvp(p)   && !Mkontp(p)   &&  Mprimp(p)   && !Mvectorp(p),   "kinds: prim");
    CHECK(!Mclosurep(v)   && !Menvp(v)   && !Mkontp(v)   && !Mprimp(v)   &&  Mvectorp(v),   "kinds: vector");

    /* Procedures: closure, prim, kont. (A kont is what call/cc hands
     * back to user code, and is invoked like any other procedure.) */
    CHECK(Mprocedurep(clo), "proc: closure is a procedure");
    CHECK(Mprocedurep(p),   "proc: prim is a procedure");
    CHECK(Mprocedurep(k),   "proc: kont is a procedure");
    CHECK(!Mprocedurep(env), "proc: env is not a procedure");
    CHECK(!Mprocedurep(v),   "proc: vector is not a procedure");
    CHECK(!Mprocedurep(Mfixnum(0)), "proc: fixnum is not a procedure");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

/* -----------------------------------------------------------------------
 * String allocation, indexing, and disjointness from the other typed-obj
 * kinds. v1 is ASCII-only, so every byte we exercise stays in 0..0x7F.
 * --------------------------------------------------------------------- */

static void test_string_smoke(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    /* Declare-then-protect-then-assign so stress mode can't strand
     * an earlier allocation across a later one. */
    mobj empty = Mnull, s = Mnull, hello = Mnull;
    MINIM_GC_PROTECT(empty);
    MINIM_GC_PROTECT(s);
    MINIM_GC_PROTECT(hello);

    empty = Mstring(0, 'x');
    CHECK(Mstringp(empty),                  "string: empty stringp");
    CHECK(Mstring_length(empty) == 0,       "string: empty length 0");

    /* `Mstring(N, c)` fills every byte. */
    s = Mstring(5, 'a');
    CHECK(Mstring_length(s) == 5,           "string: Mstring length");
    for (size_t i = 0; i < 5; i++)
        CHECK(Mstring_ref(s, i) == 'a',     "string: Mstring fill");

    /* `Mstring_set` / `Mstring_ref` round-trip. */
    Mstring_set(s, 2, 'Z');
    CHECK(Mstring_ref(s, 2) == 'Z',         "string: set/ref roundtrip");
    CHECK(Mstring_ref(s, 1) == 'a',         "string: set leaves siblings alone");

    /* `Mstring_from_bytes` copies arbitrary input. */
    hello = Mstring_from_bytes("hello", 5);
    CHECK(Mstring_length(hello) == 5,       "string: from_bytes length");
    CHECK(Mstring_ref(hello, 0) == 'h',     "string: from_bytes [0]=h");
    CHECK(Mstring_ref(hello, 4) == 'o',     "string: from_bytes [4]=o");

    /* Distinct strings live at distinct addresses. */
    CHECK(s != hello,                       "string: distinct allocations");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

static void test_string_disjoint(void) {
    Minit();
    MINIM_GC_FRAME_BEGIN;

    mobj s = Mnull, v = Mnull;
    MINIM_GC_PROTECT(s);
    MINIM_GC_PROTECT(v);

    s = Mstring_from_bytes("abc", 3);
    v = Mvector(2, Mfalse);

    /* String matches only its own typed-obj predicate. */
    CHECK( Mstringp(s) && !Mvectorp(s) && !Menvp(s) && !Mkontp(s) && !Mprimp(s),
          "string: disjoint from other typed-obj kinds");
    /* And other typed objects are not strings. */
    CHECK(!Mstringp(v),                     "string: vector is not a string");
    /* Non-typed-obj values are not strings either. */
    CHECK(!Mstringp(Mfixnum(0)),            "string: fixnum is not a string");
    CHECK(!Mstringp(Mfalse),                "string: #f is not a string");
    CHECK(!Mstringp(Mchar('a')),            "string: char is not a string");
    /* Strings are not procedures. */
    CHECK(!Mprocedurep(s),                  "string: not a procedure");

    MINIM_GC_FRAME_END;
    Mshutdown();
}

int main(void) {
    test_cons_list_1000();
    test_flonum_heap_roundtrip();
    test_vector_smoke();
    test_kinds_disjoint();
    test_string_smoke();
    test_string_disjoint();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
