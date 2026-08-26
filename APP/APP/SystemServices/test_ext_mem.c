/* ================================================================
 * test_ext_mem.c —— ext_mem 统一内存池单元测试（主机运行）
 *
 * 运行（本目录）：
 *   gcc -std=c99 -O2 -DEXT_MEM_NO_OS -I. -I../Application/ctrl \
 *       test_ext_mem.c ext_mem.c -o test_ext_mem
 *
 * 测试策略（验证分配器核心性质，非实现细节）：
 *   对齐 / 重用 / 分裂合并 / 耗尽 / 重分配 / canary 越界捕获 /
 *   双重释放防护 / 非法指针防御 / 随机压力碎片 / 参数边界（含溢出）
 * ================================================================ */
#include "mini_test.h"
#include "ext_mem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define POOL_SIZE (256u * 1024u)

static uint8_t s_pool[POOL_SIZE] __attribute__((aligned(8)));

/* ---------------- 初始化与统计基线 ---------------- */
static void test_init_basic(void)
{
    TEST_ASSERT_TRUE(ExtMem_Init(s_pool, POOL_SIZE) == 0);
    ext_mem_stats_t st;
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.total == POOL_SIZE);
    TEST_ASSERT_TRUE(st.used == 0u);
    TEST_ASSERT_TRUE(st.max_free >= POOL_SIZE - 16u);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 基本分配/释放/地址重用 ---------------- */
static void test_alloc_free_reuse(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    void *a = ExtMem_Alloc(100);
    TEST_ASSERT_TRUE(a != NULL);
    memset(a, 0xAB, 100);
    void *b = ExtMem_Alloc(100);
    TEST_ASSERT_TRUE(b != NULL && b != a);
    ExtMem_Free(a);
    void *c = ExtMem_Alloc(100);            /* first-fit：应复用 a 的地址 */
    TEST_ASSERT_TRUE(c == a);
    ExtMem_Free(b);
    ExtMem_Free(c);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 8 字节对齐（LVGL/FPU 要求） ---------------- */
static void test_alignment(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    for (int i = 1; i <= 300; i++) {
        void *p = ExtMem_Alloc((uint32_t)(i * 7 + 1));
        TEST_ASSERT_TRUE(p != NULL);
        TEST_ASSERT_TRUE(((uintptr_t)p & 7u) == 0u);
        ExtMem_Free(p);
    }
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 分裂与双向合并（边界标记） ---------------- */
static void test_split_coalesce(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    void *a = ExtMem_Alloc(1024);
    void *b = ExtMem_Alloc(1024);
    TEST_ASSERT_TRUE(a != NULL && b != NULL);
    ExtMem_Free(a);
    ExtMem_Free(b);                         /* 逆序释放：前/后合并均应生效 */
    ext_mem_stats_t st;
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.used == 0u);
    TEST_ASSERT_TRUE(st.max_free >= POOL_SIZE - 16u);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 全池耗尽 + 峰值统计 ---------------- */
static void test_exhaust(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    void *ptrs[512];
    int n = 0;
    for (; n < 512; n++) {
        ptrs[n] = ExtMem_Alloc(1024);
        if (ptrs[n] == NULL) {
            break;
        }
    }
    TEST_ASSERT_TRUE(n > 100);
    ext_mem_stats_t st;
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.fail_cnt >= 1u);
    TEST_ASSERT_TRUE(st.used <= st.total);
    TEST_ASSERT_TRUE(st.peak >= 250u * 1024u);
    for (int i = 0; i < n; i++) {
        ExtMem_Free(ptrs[i]);
    }
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.used == 0u);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 重分配：扩容搬移保留数据 / 缩容原地 ---------------- */
static void test_realloc(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    void *p = ExtMem_Alloc(64);
    TEST_ASSERT_TRUE(p != NULL);
    memset(p, 0x11, 64);
    void *q = ExtMem_Realloc(p, 4096);
    TEST_ASSERT_TRUE(q != NULL);
    TEST_ASSERT_TRUE(((uint8_t *)q)[0] == 0x11u);
    TEST_ASSERT_TRUE(((uint8_t *)q)[63] == 0x11u);
    void *r = ExtMem_Realloc(q, 32);
    TEST_ASSERT_TRUE(r == q);
    ExtMem_Free(r);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- canary 越界捕获 + 双重释放 + 非法指针防御 ---------------- */
static void test_canary_double_free(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    void *p = ExtMem_Alloc(64);
    TEST_ASSERT_TRUE(p != NULL);
    ((uint8_t *)p)[64 + 4] = 0x00;          /* 破坏尾部 canary */
    ExtMem_Free(p);
    ext_mem_stats_t st;
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.canary_fail >= 1u);

    void *q = ExtMem_Alloc(128);
    TEST_ASSERT_TRUE(q != NULL);
    ExtMem_Free(q);
    ExtMem_Free(q);                         /* 双重释放：计数且不崩溃 */
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.canary_fail >= 2u);

    /* 非法指针防御：Realloc 传入池外指针 → 返回 NULL 且不越界拷贝 */
    void *bad = (void *)((uint8_t *)s_pool - 64u);
    TEST_ASSERT_TRUE(ExtMem_Realloc(bad, 128) == NULL);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

/* ---------------- 随机压力：碎片化后全回收 ---------------- */
static void test_stress_frag(void)
{
    ExtMem_Init(s_pool, POOL_SIZE);
    srand(42);
    void *ptrs[64];
    int n = 0;
    for (int round = 0; round < 5000; round++) {
        if (n < 64 && rand() % 3 != 0) {
            uint32_t sz = (uint32_t)(rand() % 2048) + 8u;
            void *p = ExtMem_Alloc(sz);
            if (p != NULL) {
                memset(p, 0x5A, sz);
                ptrs[n++] = p;
            }
        } else if (n > 0) {
            int idx = rand() % n;
            ExtMem_Free(ptrs[idx]);
            ptrs[idx] = ptrs[--n];
        }
    }
    for (int i = 0; i < n; i++) {
        ExtMem_Free(ptrs[i]);
    }
    ext_mem_stats_t st;
    ExtMem_GetStats(&st);
    TEST_ASSERT_TRUE(st.used == 0u);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
    void *big = ExtMem_Alloc(POOL_SIZE / 2);
    TEST_ASSERT_TRUE(big != NULL);
    ExtMem_Free(big);
}

/* ---------------- 参数边界（含 32 位回绕防护） ---------------- */
static void test_bad_args(void)
{
    TEST_ASSERT_TRUE(ExtMem_Init(NULL, 100) != 0);
    TEST_ASSERT_TRUE(ExtMem_Init(s_pool, 24) != 0);
    TEST_ASSERT_TRUE(ExtMem_Init((void *)((uint8_t *)s_pool + 4), POOL_SIZE) != 0);
    ExtMem_Init(s_pool, POOL_SIZE);
    TEST_ASSERT_TRUE(ExtMem_Alloc(0) == NULL);
    TEST_ASSERT_TRUE(ExtMem_Alloc(0xFFFFFFFFu) == NULL);
    TEST_ASSERT_TRUE(ExtMem_Alloc(POOL_SIZE * 2u) == NULL);
    ExtMem_Free(NULL);
    void *rp = ExtMem_Realloc(NULL, 64);
    TEST_ASSERT_TRUE(rp != NULL);
    ExtMem_Free(rp);
    TEST_ASSERT_TRUE(ExtMem_CheckIntegrity() == 0);
}

int main(void)
{
    RUN_TEST(test_init_basic);
    RUN_TEST(test_alloc_free_reuse);
    RUN_TEST(test_alignment);
    RUN_TEST(test_split_coalesce);
    RUN_TEST(test_exhaust);
    RUN_TEST(test_realloc);
    RUN_TEST(test_canary_double_free);
    RUN_TEST(test_stress_frag);
    RUN_TEST(test_bad_args);
    TEST_SUMMARY();
}
