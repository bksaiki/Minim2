#ifndef MINIM_TEST_HARNESS_H
#define MINIM_TEST_HARNESS_H

#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                           \
    do {                                           \
        tests_run++;                               \
        if (!(cond)) {                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", \
                    __FILE__, __LINE__, (msg));     \
            tests_failed++;                        \
        }                                          \
    } while (0)

#define TEST_REPORT()                                        \
    do {                                                     \
        if (tests_failed == 0)                               \
            printf("All %d test(s) passed.\n", tests_run);  \
        else                                                 \
            printf("%d/%d test(s) FAILED.\n",               \
                   tests_failed, tests_run);                 \
    } while (0)

#endif /* MINIM_TEST_HARNESS_H */
