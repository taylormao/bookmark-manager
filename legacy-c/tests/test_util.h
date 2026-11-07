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
static int g_skips = 0;

/*
 * 跳过一个用例：记录次数并打印 SKIP 行。
 * 只 printf 不计数会让 test_report 输出「0 项断言全部通过」并由 CTest 记为
 * Pass，形成假绿；配合 test_report 的 SKIP_RETURN_CODE 让跳过显式化。
 */
#define TEST_SKIP(...)                                                       \
    do {                                                                     \
        g_skips++;                                                           \
        printf("SKIP ");                                                     \
        printf(__VA_ARGS__);                                                 \
        fputc('\n', stdout);                                                 \
    } while (0)

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

/* 77 为 CTest SKIP_RETURN_CODE（tests/CMakeLists.txt 中注册）。 */
#define BMK_TEST_SKIP_RETURN_CODE 77

static int test_report(const char *suite)
{
    if (g_fails > 0) {
        printf("[失败] %s: %d/%d 项断言失败\n", suite, g_fails, g_tests);
        return 1;
    }
    if (g_skips > 0) {
        printf("[跳过] %s: %d 项断言通过，%d 项用例被跳过（覆盖不完整）\n",
               suite, g_tests, g_skips);
        return BMK_TEST_SKIP_RETURN_CODE;
    }
    printf("[通过] %s: %d 项断言全部通过\n", suite, g_tests);
    return 0;
}

#endif /* BMK_TEST_UTIL_H */
