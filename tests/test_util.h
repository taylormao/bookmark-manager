/*
 * test_util.h - 极简断言框架（无外部依赖，需求 7.1）
 */
#ifndef BMK_TEST_UTIL_H
#define BMK_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_tests++;                                                           \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expect, ...)                                       \
    do {                                                                     \
        const char *a_ = (actual), *e_ = (expect);                           \
        g_tests++;                                                           \
        if (!a_ || !e_ || strcmp(a_, e_) != 0) {                             \
            g_fails++;                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  期望: %s\n  实际: %s\n",                    \
                    e_ ? e_ : "(null)", a_ ? a_ : "(null)");                 \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expect, ...)                                        \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expect);        \
        g_tests++;                                                           \
        if (a_ != e_) {                                                      \
            g_fails++;                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  期望: %lld\n  实际: %lld\n", e_, a_);       \
        }                                                                    \
    } while (0)

static int test_report(const char *suite)
{
    if (g_fails == 0) {
        printf("[通过] %s: %d 项断言全部通过\n", suite, g_tests);
        return 0;
    }
    printf("[失败] %s: %d/%d 项断言失败\n", suite, g_fails, g_tests);
    return 1;
}

#endif /* BMK_TEST_UTIL_H */
