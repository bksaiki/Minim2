#include "minim.h"
#include "harness.h"

/* -----------------------------------------------------------------------
 * Phase 0 — scaffolding
 * --------------------------------------------------------------------- */

static void test_init_shutdown(void) {
    Minit();
    Mshutdown();
    CHECK(1, "init/shutdown smoke test");
}

/* -----------------------------------------------------------------------
 * Phase 1 — pure-C round-trip tests (no allocation)
 * --------------------------------------------------------------------- */

static void test_fixnum_roundtrip(void) {
    intptr_t values[] = { 0, 1, -1, 42, -42, (intptr_t)((1LL << 60) - 1), -(1LL << 60) };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        mobj v = Mfixnum(values[i]);
        CHECK(Mfixnump(v), "fixnum: tag check");
        CHECK(!Mpairp(v), "fixnum: not pair");
        CHECK(Mfixnum_val(v) == values[i], "fixnum: value round-trip");
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
    CHECK(Mfalsep(Mfalse), "false: falsep");
    CHECK(!Mtruep(Mfalse), "false: not truep");
    CHECK(!Mnullp(Mfalse), "false: not nullp");
    CHECK(!Meofp(Mfalse), "false: not eofp");

    CHECK(Mtruep(Mtrue), "true: truep");
    CHECK(!Mfalsep(Mtrue), "true: not falsep");
    CHECK(!Meofp(Mtrue), "true: not eofp");

    CHECK(Mnullp(Mnull), "null: nullp");
    CHECK(!Mfalsep(Mnull), "null: not falsep");
    CHECK(!Mtruep(Mnull), "null: not truep");
    CHECK(!Meofp(Mnull), "null: not eofp");

    CHECK(Meofp(Meof), "eof: eofp");
    CHECK(!Mfalsep(Meof), "eof: not falsep");
    CHECK(!Mtruep(Meof), "eof: not truep");
    CHECK(!Mnullp(Meof), "eof: not nullp");

    CHECK(Mbooleanp(Mtrue), "booleanp: #t");
    CHECK(Mbooleanp(Mfalse), "booleanp: #f");
    CHECK(!Mbooleanp(Mnull), "booleanp: not null");
    CHECK(!Mbooleanp(Meof), "booleanp: not eof");
}

static void test_immediate_values(void) {
    CHECK(Mfalse == (mobj)0x06, "MFALSE == 0x06");
    CHECK(Mtrue == (mobj)0x0E, "MTRUE  == 0x0E");
    CHECK(Mnull == (mobj)0x26, "MNULL  == 0x26");
    CHECK(Meof  == (mobj)0x36, "MEOF   == 0x36");
    CHECK(MFORWARD_MARKER == (mobj)0x3E, "MFORWARD_MARKER == 0x3E");

    /* All four immediates must be distinct */
    CHECK(Mtrue != Mfalse, "immediates: #t != #f");
    CHECK(Mnull != Mfalse, "immediates: () != #f");
    CHECK(Mnull != Mtrue,  "immediates: () != #t");
    CHECK(Meof  != Mfalse, "immediates: eof != #f");
    CHECK(Meof  != Mtrue,  "immediates: eof != #t");
    CHECK(Meof  != Mnull,  "immediates: eof != ()");

    /* All four must carry MTAG_IMMEDIATE */
    CHECK((Mtrue  & MTAG_MASK) == MTAG_IMMEDIATE, "true: tag IMMEDIATE");
    CHECK((Mfalse & MTAG_MASK) == MTAG_IMMEDIATE, "false: tag IMMEDIATE");
    CHECK((Mnull  & MTAG_MASK) == MTAG_IMMEDIATE, "null: tag IMMEDIATE");
    CHECK((Meof   & MTAG_MASK) == MTAG_IMMEDIATE, "eof: tag IMMEDIATE");
}

static void test_fixnum_zero_tag(void) {
    /* Fixnum tag is 0 — so fixnum arithmetic works without untagging */
    mobj a = Mfixnum(3);
    mobj b = Mfixnum(5);
    mobj sum = (mobj)(a + b);
    CHECK(Mfixnump(sum), "fixnum: sum is fixnum");
    CHECK(Mfixnum_val(sum) == 8, "fixnum: 3+5==8 without untag");
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
