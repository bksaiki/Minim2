#include "minim.h"

#include <stdio.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Minimal test harness
 * --------------------------------------------------------------------- */

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        tests_run++;                                              \
        if (!(cond)) {                                            \
            fprintf(stderr, "FAIL [%s:%d]: %s\n",                \
                    __FILE__, __LINE__, (msg));                   \
            tests_failed++;                                       \
        }                                                         \
    } while (0)

/* -----------------------------------------------------------------------
 * Phase 1 — pure-C round-trip tests (no allocation)
 * --------------------------------------------------------------------- */

static void test_fixnum_roundtrip(void) {
    intptr_t values[] = { 0, 1, -1, 42, -42, (intptr_t)((1LL << 60) - 1), -(1LL << 60) };
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        mobj v = minim_make_fixnum(values[i]);
        CHECK(minim_fixnump(v),             "fixnum: tag check");
        CHECK(!minim_pairp(v),              "fixnum: not pair");
        CHECK(!minim_immediatep(v),         "fixnum: not immediate");
        CHECK(minim_fixnum_value(v) == values[i], "fixnum: value round-trip");
    }
}

static void test_tags_dont_overlap(void) {
    /* Distinct tag constants must all be different */
    CHECK(MTAG_FIXNUM    != MTAG_PAIR,      "tags: fixnum != pair");
    CHECK(MTAG_PAIR      != MTAG_FLONUM,    "tags: pair != flonum");
    CHECK(MTAG_FLONUM    != MTAG_SYMBOL,    "tags: flonum != symbol");
    CHECK(MTAG_SYMBOL    != MTAG_IMMEDIATE, "tags: symbol != immediate");
    CHECK(MTAG_IMMEDIATE != MTAG_TYPED_OBJ, "tags: immediate != typed_obj");
}

static void test_immediate_constants(void) {
    CHECK(minim_falsep(MINIM_FALSE),        "false: falsep");
    CHECK(minim_immediatep(MINIM_FALSE),    "false: immediatep");
    CHECK(!minim_truep(MINIM_FALSE),        "false: not truep");
    CHECK(!minim_nullp(MINIM_FALSE),        "false: not nullp");

    CHECK(minim_truep(MINIM_TRUE),          "true: truep");
    CHECK(minim_immediatep(MINIM_TRUE),     "true: immediatep");
    CHECK(!minim_falsep(MINIM_TRUE),        "true: not falsep");

    CHECK(minim_nullp(MINIM_NULL),          "null: nullp");
    CHECK(minim_immediatep(MINIM_NULL),     "null: immediatep");
    CHECK(!minim_falsep(MINIM_NULL),        "null: not falsep");
    CHECK(!minim_truep(MINIM_NULL),         "null: not truep");

    /* booleanp matches exactly #t and #f */
    CHECK(minim_booleanp(MINIM_TRUE),       "booleanp: #t");
    CHECK(minim_booleanp(MINIM_FALSE),      "booleanp: #f");
    CHECK(!minim_booleanp(MINIM_NULL),      "booleanp: not null");
}

static void test_immediate_values(void) {
    CHECK(MINIM_FALSE == (mobj)0x06,        "MINIM_FALSE == 0x06");
    CHECK(MINIM_TRUE  == (mobj)0x0E,        "MINIM_TRUE  == 0x0E");
    CHECK(MINIM_NULL  == (mobj)0x16,        "MINIM_NULL  == 0x16");
    CHECK(MFORWARD_MARKER == (mobj)0x3E,    "MFORWARD_MARKER == 0x3E");
}

static void test_fixnum_zero_tag(void) {
    /* Fixnum tag is 0 — so fixnum arithmetic works without untagging */
    mobj a = minim_make_fixnum(3);
    mobj b = minim_make_fixnum(5);
    mobj sum = (mobj)(a + b);
    CHECK(minim_fixnump(sum),               "fixnum: sum is fixnum");
    CHECK(minim_fixnum_value(sum) == 8,     "fixnum: 3+5==8 without untag");
}

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
    CHECK(ok,             "cons list: all 1000 values correct");
    CHECK(expected == -1, "cons list: exactly 1000 elements");

    MINIM_GC_FRAME_END;
    minim_shutdown();
}

static void test_flonum_heap_roundtrip(void) {
    minim_init();
    mobj v = minim_make_flonum(3.14159);
    CHECK(minim_flonump(v),   "flonum: tag");
    CHECK(minim_flonum_value(v) == 3.14159, "flonum: value round-trip");
    minim_shutdown();
}

static void test_vector_smoke(void) {
    minim_init();
    MINIM_GC_FRAME_BEGIN;

    mobj v = minim_make_vector(10, minim_make_fixnum(0));
    MINIM_GC_PROTECT(v);

    CHECK(minim_vectorp(v),              "vector: vectorp");
    CHECK(minim_vector_length(v) == 10,  "vector: length 10");
    for (size_t i = 0; i < 10; i++)
        CHECK(minim_fixnum_value(minim_vector_ref(v, i)) == 0,
              "vector: fill is 0");

    minim_vector_set(v, 5, minim_make_fixnum(42));
    CHECK(minim_fixnum_value(minim_vector_ref(v, 5)) == 42,
          "vector: set/ref round-trip");

    MINIM_GC_FRAME_END;
    minim_shutdown();
}

/* -----------------------------------------------------------------------
 * Phase 0 scaffolding smoke test
 * --------------------------------------------------------------------- */

static void test_init_shutdown(void) {
    minim_init();
    minim_shutdown();
    CHECK(1, "init/shutdown smoke test");
}

int main(void) {
    test_fixnum_roundtrip();
    test_tags_dont_overlap();
    test_immediate_constants();
    test_immediate_values();
    test_fixnum_zero_tag();
    test_init_shutdown();
    test_cons_list_1000();
    test_flonum_heap_roundtrip();
    test_vector_smoke();

    if (tests_failed == 0)
        printf("All %d test(s) passed.\n", tests_run);
    else
        printf("%d/%d test(s) FAILED.\n", tests_failed, tests_run);

    return tests_failed ? 1 : 0;
}
