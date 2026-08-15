/* BOOT 纯逻辑服务层主机单元测试：crc32 + fifo */
#include <stdio.h>
#include <string.h>
#include "crc32.h"
#include "fifo.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void test_crc32_known_vector(void)
{
    uint32_t crc = 0;
    crc32_init(&crc);
    crc32_update(&crc, (const uint8_t *)"123456789", 9);
    uint32_t result = crc32_finalize(&crc);
    CHECK(result == 0xCBF43926UL, "CRC-32 标准向量 0xCBF43926");
}

static void test_crc32_empty(void)
{
    uint32_t crc = 0;
    crc32_init(&crc);
    CHECK(crc32_finalize(&crc) == 0x00000000UL, "CRC-32 空数据 = 0");
}

static void test_fifo_basic(void)
{
    fifo_t f;
    uint8_t b = 0;
    fifo_init(&f);
    CHECK(fifo_available(&f) == 0, "初始为空");
    CHECK(fifo_put(&f, 0xAA) == true, "写入成功");
    CHECK(fifo_available(&f) == 1, "可用数=1");
    CHECK(fifo_get(&f, &b) == true && b == 0xAA, "读回正确");
    CHECK(fifo_available(&f) == 0, "读后为空");
}

static void test_fifo_wrap_and_full(void)
{
    fifo_t f;
    fifo_init(&f);

    /* 写满（容量 = FIFO_SIZE-1，环空/满判定占一格） */
    int put_ok = 1;
    for (int i = 0; i < FIFO_SIZE - 1; i++) {
        if (!fifo_put(&f, (uint8_t)i)) { put_ok = 0; break; }
    }
    CHECK(put_ok == 1, "可写满 FIFO_SIZE-1");
    CHECK(fifo_available(&f) == FIFO_SIZE - 1, "满时可用数=FIFO_SIZE-1");
    CHECK(fifo_put(&f, 0x99) == false, "满时写入被拒");

    /* 环绕：读出前半再写入，验证 head/tail 环绕正确 */
    uint8_t b = 0;
    for (int i = 0; i < FIFO_SIZE / 2; i++) fifo_get(&f, &b);
    CHECK(b == (uint8_t)(FIFO_SIZE / 2 - 1), "环绕前半读值正确");
    CHECK(fifo_put(&f, 0x55) == true, "环绕后写入成功");
    fifo_get(&f, &b);
    CHECK(b == (uint8_t)(FIFO_SIZE / 2), "环绕后读值正确");
}

int services_test_main(void)
{
    printf("== BOOT 服务层主机测试 ==\n");
    test_crc32_known_vector();
    test_crc32_empty();
    test_fifo_basic();
    test_fifo_wrap_and_full();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST(S) FAILED\n", failures);
    return 1;
}
