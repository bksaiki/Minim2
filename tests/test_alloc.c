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

int main(void) {
    test_cons_list_1000();
    test_flonum_heap_roundtrip();
    test_vector_smoke();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
