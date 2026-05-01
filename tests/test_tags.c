#include "minim.h"
#include "harness.h"

/* -----------------------------------------------------------------------
 * Phase 0 — scaffolding
 * --------------------------------------------------------------------- */

static void test_init_shutdown(void) {
    minim_init();
    minim_shutdown();
    CHECK(1, "init/shutdown smoke test");
}

/* -----------------------------------------------------------------------
 * Phase 1 — pure-C round-trip tests (no allocation)
 * --------------------------------------------------------------------- */

static void test_fixnum_roundtrip(void) {
    intptr_t values[] = { 0, 1, -1, 42, -42, (intptr_t)((1LL << 60) - 1), -(1LL << 60) };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        mobj v = minim_make_fixnum(values[i]);
        CHECK(minim_fixnump(v), "fixnum: tag check");
        CHECK(!minim_pairp(v), "fixnum: not pair");
        CHECK(!minim_immediatep(v), "fixnum: not immediate");
        CHECK(minim_fixnum_value(v) == values[i], "fixnum: value round-trip");
    }
}

static void test_tags_dont_overlap(void) {
    CHECK(MTAG_FIXNUM != MTAG_PAIR, "tags: fixnum != pair");
    CHECK(MTAG_PAIR != MTAG_FLONUM, "tags: pair != flonum");
    CHECK(MTAG_FLONUM != MTAG_SYMBOL, "tags: flonum != symbol");
    CHECK(MTAG_SYMBOL != MTAG_IMMEDIATE, "tags: symbol != immediate");
    CHECK(MTAG_IMMEDIATE != MTAG_TYPED_OBJ, "tags: immediate != typed_obj");
}

static void test_immediate_constants(void) {
    CHECK(minim_falsep(MINIM_FALSE), "false: falsep");
    CHECK(minim_immediatep(MINIM_FALSE), "false: immediatep");
    CHECK(!minim_truep(MINIM_FALSE), "false: not truep");
    CHECK(!minim_nullp(MINIM_FALSE), "false: not nullp");

    CHECK(minim_truep(MINIM_TRUE), "true: truep");
    CHECK(minim_immediatep(MINIM_TRUE), "true: immediatep");
    CHECK(!minim_falsep(MINIM_TRUE), "true: not falsep");

    CHECK(minim_nullp(MINIM_NULL), "null: nullp");
    CHECK(minim_immediatep(MINIM_NULL), "null: immediatep");
    CHECK(!minim_falsep(MINIM_NULL), "null: not falsep");
    CHECK(!minim_truep(MINIM_NULL), "null: not truep");

    CHECK(minim_booleanp(MINIM_TRUE), "booleanp: #t");
    CHECK(minim_booleanp(MINIM_FALSE), "booleanp: #f");
    CHECK(!minim_booleanp(MINIM_NULL), "booleanp: not null");
}

static void test_immediate_values(void) {
    CHECK(MINIM_FALSE == (mobj)0x06, "MINIM_FALSE == 0x06");
    CHECK(MINIM_TRUE == (mobj)0x0E, "MINIM_TRUE  == 0x0E");
    CHECK(MINIM_NULL == (mobj)0x16, "MINIM_NULL  == 0x16");
    CHECK(MFORWARD_MARKER == (mobj)0x3E, "MFORWARD_MARKER == 0x3E");
}

static void test_fixnum_zero_tag(void) {
    /* Fixnum tag is 0 — so fixnum arithmetic works without untagging */
    mobj a = minim_make_fixnum(3);
    mobj b = minim_make_fixnum(5);
    mobj sum = (mobj)(a + b);
    CHECK(minim_fixnump(sum), "fixnum: sum is fixnum");
    CHECK(minim_fixnum_value(sum) == 8, "fixnum: 3+5==8 without untag");
}

int main(void) {
    test_init_shutdown();
    test_fixnum_roundtrip();
    test_tags_dont_overlap();
    test_immediate_constants();
    test_immediate_values();
    test_fixnum_zero_tag();
    TEST_REPORT();
    return tests_failed ? 1 : 0;
}
