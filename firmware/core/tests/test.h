#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;

#define TEST_ASSERT(cond, msg) do {                          \
    if (!(cond)) {                                             \
        fprintf(stderr, "FAIL: %s (%s:%d)\n",                  \
                msg, __FILE__, __LINE__);                        \
        test_failures++;                                       \
    } else {                                                   \
        printf(".");                                            \
    }                                                          \
} while (0)

#define TEST_RUN(fn) do {                                     \
    printf("Running %s... ", #fn);                             \
    fn();                                                        \
    printf("ok\n");                                             \
} while (0)

#endif // TEST_H
