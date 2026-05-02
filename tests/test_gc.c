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

int main(void) {
    test_cons_survives_gc();
    test_vector_survives_gc();
    test_circular_list_gc();
    test_intern_identity_across_gc();
    test_heap_growth();
    test_shutdown_resets_intern_table();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
