/* ================================================================
 * mini_test.h —— 轻量单元测试框架（无依赖，主机/嵌入式皆可）
 *
 * 用法：
 *   #include "mini_test.h"
 *   static void test_xxx(void) { TEST_ASSERT_NEAR(1.0f, got, 1e-3f); }
 *   int main(void) { RUN_TEST(test_xxx); TEST_SUMMARY(); return 0; }
 *
 * 输出：PASS/FAIL 逐项打印 + 汇总；返回码 0=全过 1=有失败。
 * ================================================================ */
#ifndef MINI_TEST_H
#define MINI_TEST_H

#include <stdio.h>
#include <math.h>

static int test_pass_cnt = 0;
static int test_fail_cnt = 0;
static int test_cur_fail = 0;

#define TEST_ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            test_cur_fail = 1; \
            printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define TEST_ASSERT_NEAR(a, b, tol) \
    do { \
        double _a = (double)(a), _b = (double)(b); \
        if (fabs(_a - _b) > (double)(tol)) { \
            test_cur_fail = 1; \
            printf("    [FAIL] %s:%d  %s=%.6f vs %s=%.6f (tol %.6f)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b, (double)(tol)); \
        } \
    } while (0)

#define TEST_ASSERT_NEAR_MSG(a, b, tol, msg) \
    do { \
        double _a = (double)(a), _b = (double)(b); \
        if (fabs(_a - _b) > (double)(tol)) { \
            test_cur_fail = 1; \
            printf("    [FAIL] %s:%d  %s (%.6f vs %.6f)\n", \
                   __FILE__, __LINE__, msg, _a, _b); \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        test_cur_fail = 0; \
        printf("[TEST] %s\n", #fn); \
        fn(); \
        if (test_cur_fail) { test_fail_cnt++; printf("  -> FAIL\n"); } \
        else { test_pass_cnt++; printf("  -> PASS\n"); } \
    } while (0)

#define TEST_SUMMARY() \
    do { \
        printf("\n================ SUMMARY ================\n"); \
        printf("PASS: %d   FAIL: %d\n", test_pass_cnt, test_fail_cnt); \
        return (test_fail_cnt == 0) ? 0 : 1; \
    } while (0)

#endif /* MINI_TEST_H */
