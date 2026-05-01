#include "minim.h"
#include "harness.h"

/* -----------------------------------------------------------------------
 * Phase 2 — bump allocator smoke tests (no GC triggered)
 * --------------------------------------------------------------------- */

static void test_cons_list_1000(void) {
    minim_init();

    /* Build a list: (999 998 ... 1 0) */
    mobj lst = MINIM_NULL;
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(lst);
    for (int i = 0; i < 1000; i++)
        lst = minim_cons(minim_make_fixnum(i), lst);

    /* Walk and verify values descend from 999 to 0 */
    int expected = 999;
    int ok = 1;
    mobj cur = lst;
    while (!minim_nullp(cur)) {
        CHECK(minim_pairp(cur), "cons list: element is pair");
        if (minim_fixnum_value(minim_car(cur)) != expected)
            ok = 0;
        cur = minim_cdr(cur);
        expected--;
    }
    CHECK(ok, "cons list: all 1000 values correct");
    CHECK(expected == -1, "cons list: exactly 1000 elements");

    MINIM_GC_FRAME_END;
    minim_shutdown();
}

static void test_flonum_heap_roundtrip(void) {
    minim_init();
    mobj v = minim_make_flonum(3.14159);
    CHECK(minim_flonump(v), "flonum: tag");
    CHECK(minim_flonum_value(v) == 3.14159, "flonum: value round-trip");
    minim_shutdown();
}

static void test_vector_smoke(void) {
    minim_init();
    MINIM_GC_FRAME_BEGIN;

    mobj v = minim_make_vector(10, minim_make_fixnum(0));
    MINIM_GC_PROTECT(v);

    CHECK(minim_vectorp(v), "vector: vectorp");
    CHECK(minim_vector_length(v) == 10, "vector: length 10");
    for (size_t i = 0; i < 10; i++)
        CHECK(minim_fixnum_value(minim_vector_ref(v, i)) == 0, "vector: fill is 0");

    minim_vector_set(v, 5, minim_make_fixnum(42));
    CHECK(minim_fixnum_value(minim_vector_ref(v, 5)) == 42, "vector: set/ref round-trip");

    MINIM_GC_FRAME_END;
    minim_shutdown();
}

int main(void) {
    test_cons_list_1000();
    test_flonum_heap_roundtrip();
    test_vector_smoke();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
