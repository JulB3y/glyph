#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_failures;
static int test_checks;

#define ASSERT_TRUE(expr) do { \
    test_checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        test_failures++; \
    } \
} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) do { \
    test_checks++; \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); \
        test_failures++; \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    test_checks++; \
    const char *_a = (a), *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, \
                _a ? _a : "(null)", _b ? _b : "(null)"); \
        test_failures++; \
    } \
} while (0)

#define ASSERT_NOT_NULL(expr) do { \
    test_checks++; \
    if ((expr) == NULL) { \
        fprintf(stderr, "FAIL %s:%d: %s != NULL\n", __FILE__, __LINE__, #expr); \
        test_failures++; \
    } \
} while (0)

#define ASSERT_NULL(expr) do { \
    test_checks++; \
    if ((expr) != NULL) { \
        fprintf(stderr, "FAIL %s:%d: %s == NULL\n", __FILE__, __LINE__, #expr); \
        test_failures++; \
    } \
} while (0)

#define TEST_SUMMARY() do { \
    fprintf(stderr, "%d checks, %d failures\n", test_checks, test_failures); \
} while (0)

#endif /* TEST_COMMON_H */
