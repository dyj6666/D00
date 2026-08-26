/* ================================================================
 * bsp_sram —— 板载外部 SRAM（IS62WV51216 1MB，FSMC NE3）驱动实现
 *
 * 架构位置：APP BSP 层。
 *
 * 自检策略（上电一次，~10ms 级）：
 *   - 数据线：16 位走位（0x0001 逐位左移）——抓相邻数据线短路/断路；
 *   - 地址线：低位 2B 步进 + 高位 0x800 步进写唯一值回读——抓地址
 *     译码错误（焊接虚焊/短路）；
 *   - 模式：0x5555/0xAAAA/全 0/全 1 + LCG 伪随机写读——抓单元粘连。
 * 自检时机：模块 priority=2，早于 LA_Sample(30)/GuiApp(55)，全区
 * 可写；随后 ExtMem_Init 清零池，无残留污染。
 *
 * 基准策略（DWT @168MHz 周期计时）：32/16 位读写 + 32 位对齐拷贝，
 * 256KB 样本，结果入启动日志与 GUI SRAM 页。
 * ================================================================ */
#include "bsp_sram.h"

#include <string.h>

#include "bsp_system.h"     /* BSP_DWT_Enable（幂等） */
#include "ext_mem.h"
#include "logger.h"
#include "mem_map.h"
#include "stm32f4xx.h"      /* FSMC_Bank1 / DWT */

#define SRAM_BENCH_BYTES    (256u * 1024u)   /* 基准样本 256KB（池内） */
#define SRAM_COPY_BYTES     (128u * 1024u)   /* 拷贝样本 128KB */

static bsp_sram_status_t s_status;
static volatile uint32_t s_bench_sink;       /* 吞掉基准结果，防优化 */

/* ---------------- 自检：数据线（16 位走位） ---------------- */
static uint32_t test_databus(void)
{
    volatile uint16_t *p = (volatile uint16_t *)MEM_EXT_POOL_BASE;
    for (uint32_t i = 0u; i < 16u; i++) {
        uint16_t pat = (uint16_t)(1u << i);
        p[0] = pat;
        if (p[0] != pat) {
            return (1u << i);                /* 返回失败的数据线位 */
        }
    }
    return 0u;
}

/* ---------------- 自检：地址线（低位 2B + 高位 0x800 步进） ---------------- */
static uint32_t test_addrbuses(void)
{
    volatile uint8_t *base = (volatile uint8_t *)EXT_SRAM_BASE;
    const uint32_t n = EXT_SRAM_SIZE;        /* 逐字节上限 */
    uint32_t i;

    /* 低地址段：A0-A10，2B 步进（32 点覆盖） */
    for (i = 0u; i < 32u; i++) {
        base[i * 2u] = (uint8_t)(i + 1u);
    }
    for (i = 0u; i < 32u; i++) {
        if (base[i * 2u] != (uint8_t)(i + 1u)) {
            return (i + 1u);
        }
    }
    /* 高地址段：A11-A18，0x800 步进（512 点覆盖 1MB） */
    for (i = 0u; i < (n / 0x800u); i++) {
        base[i * 0x800u] = (uint8_t)((i ^ (i >> 8u)) + 1u);
    }
    for (i = 0u; i < (n / 0x800u); i++) {
        if (base[i * 0x800u] != (uint8_t)((i ^ (i >> 8u)) + 1u)) {
            return (n / 0x800u) + i + 1u;
        }
    }
    return 0u;
}

/* ---------------- 自检：模式翻转 + 伪随机 ---------------- */
static uint32_t test_patterns(void)
{
    volatile uint16_t *p = (volatile uint16_t *)MEM_EXT_POOL_BASE;
    uint32_t i;
    const uint16_t pats[4] = { 0x5555u, 0xAAAAu, 0xFFFFu, 0x0000u };

    for (uint32_t k = 0u; k < 4u; k++) {
        for (i = 0u; i < 1024u; i++) {
            p[i] = pats[k];
        }
        for (i = 0u; i < 1024u; i++) {
            if (p[i] != pats[k]) {
                return (k + 1u);
            }
        }
    }
    /* LCG 伪随机 4KB（确定性种子，可复现） */
    uint32_t lcg = 0x12345678u;
    for (i = 0u; i < 2048u; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        p[i] = (uint16_t)(lcg >> 16u);
    }
    lcg = 0x12345678u;
    for (i = 0u; i < 2048u; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        if (p[i] != (uint16_t)(lcg >> 16u)) {
            return 5u;
        }
    }
    return 0u;
}

/* ---------------- 吞吐基准（DWT 周期计数 @168MHz） ---------------- */
static uint32_t mbps(uint32_t bytes, uint32_t cycles)
{
    /* bytes × 168MHz / cycles → MB/s（整数） */
    return (uint32_t)(((uint64_t)bytes * 168u) / (cycles ? cycles : 1u));
}

static void bench_all(void)
{
    volatile uint32_t *w32 = (volatile uint32_t *)MEM_EXT_POOL_BASE;
    volatile uint16_t *w16 = (volatile uint16_t *)MEM_EXT_POOL_BASE;
    const uint32_t n32 = SRAM_BENCH_BYTES / 4u;
    const uint32_t n16 = SRAM_BENCH_BYTES / 2u;
    uint32_t t0, tc;

    /* 32 位写 */
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0u; i < n32; i++) {
        w32[i] = 0x5A5A5A5Au;
    }
    tc = DWT->CYCCNT - t0;
    s_status.write32_mbps = mbps(SRAM_BENCH_BYTES, tc);

    /* 32 位读（求和吞掉，防优化） */
    uint32_t sum = 0u;
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0u; i < n32; i++) {
        sum += w32[i];
    }
    tc = DWT->CYCCNT - t0;
    s_bench_sink = sum;
    s_status.read32_mbps = mbps(SRAM_BENCH_BYTES, tc);

    /* 16 位写/读（对照：验证 word 访问无劣势） */
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0u; i < n16; i++) {
        w16[i] = 0xA5A5u;
    }
    tc = DWT->CYCCNT - t0;
    s_status.write16_mbps = mbps(SRAM_BENCH_BYTES, tc);

    sum = 0u;
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0u; i < n16; i++) {
        sum += w16[i];
    }
    tc = DWT->CYCCNT - t0;
    s_bench_sink += sum;
    s_status.read16_mbps = mbps(SRAM_BENCH_BYTES, tc);

    /* 32 位对齐拷贝（128KB：池首 → 池首 + 128KB，总占用 256KB < 池 376KB，
     * 不越 1MB 顶；旧实现 dst=src+256KB 会越界 2KB 到空地址空间） */
    uint8_t *src = (uint8_t *)MEM_EXT_POOL_BASE;
    uint8_t *dst = src + SRAM_COPY_BYTES;
    t0 = DWT->CYCCNT;
    BSP_SRAM_Memcpy32(dst, src, SRAM_COPY_BYTES);
    tc = DWT->CYCCNT - t0;
    s_bench_sink += dst[0];
    s_status.copy_mbps = mbps(SRAM_COPY_BYTES, tc);
}

/* ---------------- 32 位对齐快速拷贝 ---------------- */
void BSP_SRAM_Memcpy32(void *dst, const void *src, uint32_t bytes)
{
    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) != 0u) {
        memcpy(dst, src, bytes);             /* 非对齐兜底 */
        return;
    }
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    uint32_t n = bytes >> 2u;

    /* 4× 展开主循环：FSMC 16 位总线下 word 访问 = 背靠背 16 位周期，
     * 循环开销摊薄到 4 次访问 */
    while (n >= 4u) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
        d += 4u;
        s += 4u;
        n -= 4u;
    }
    while (n-- > 0u) {
        *d++ = *s++;
    }
    /* 尾部字节 */
    uint32_t rem = bytes & 3u;
    if (rem != 0u) {
        uint8_t *db = (uint8_t *)d;
        const uint8_t *sb = (const uint8_t *)s;
        while (rem-- > 0u) {
            *db++ = *sb++;
        }
    }
}

const bsp_sram_status_t *BSP_SRAM_GetStatus(void)
{
    return &s_status;
}

/* ---------------- 模块初始化（module.c priority=2） ---------------- */
void BSP_SRAM_Init(void)
{
    BSP_DWT_Enable();

    /* FSMC BANK3 使能验证 + 时序回读（BCR3/BTR3 = BTCR[4]/[5]） */
    uint32_t bcr = FSMC_Bank1->BTCR[4];
    uint32_t btr = FSMC_Bank1->BTCR[5];
    s_status.fsmc_en = (uint8_t)((bcr & FSMC_BCR1_MBKEN) ? 1u : 0u);
    s_status.addset  = btr & 0xFu;
    s_status.datast  = (btr >> 8u) & 0xFu;
    LOG_Printf("[SRAM] IS62WV51216 1MB @0x%08X FSMC-NE3 BCR=0x%08X BTR=0x%08X\r\n",
               (unsigned)EXT_SRAM_BASE, (unsigned)bcr, (unsigned)btr);
    LOG_Printf("[SRAM]   bus=16bit async ADDSET=%u DATAST=%u\r\n",
               (unsigned)s_status.addset, (unsigned)s_status.datast);
    if (!s_status.fsmc_en) {
        LOG_Printf("[SRAM]   WARN: FSMC BANK3 not enabled (fsmc.c?)\r\n");
    }

    /* 上电自检（数据线 → 地址线 → 模式） */
    s_status.test_dat = (uint8_t)test_databus();
    s_status.test_adr = (uint8_t)test_addrbuses();
    s_status.test_pat = (uint8_t)test_patterns();
    LOG_Printf("[SRAM] selftest: DAT=%s ADR=%s PAT=%s\r\n",
               s_status.test_dat ? "FAIL" : "PASS",
               s_status.test_adr ? "FAIL" : "PASS",
               s_status.test_pat ? "FAIL" : "PASS");
    if (s_status.test_dat | s_status.test_adr | s_status.test_pat) {
        LOG_Printf("[SRAM]   WARN: selftest FAILED (DAT=0x%X ADR=0x%X PAT=%u); "
                   "check FSMC wiring\r\n",
                   (unsigned)s_status.test_dat, (unsigned)s_status.test_adr,
                   (unsigned)s_status.test_pat);
    }

    /* 吞吐基准（DWT） */
    bench_all();
    LOG_Printf("[SRAM] bench: W32=%u R32=%u W16=%u R16=%u CP=%u MB/s\r\n",
               (unsigned)s_status.write32_mbps, (unsigned)s_status.read32_mbps,
               (unsigned)s_status.write16_mbps, (unsigned)s_status.read16_mbps,
               (unsigned)s_status.copy_mbps);

    /* 统一内存池（LVGL 后端 + 大缓冲） */
    if (ExtMem_Init((void *)MEM_EXT_POOL_BASE, MEM_EXT_POOL_SIZE) != 0) {
        LOG_Printf("[SRAM] ExtMem_Init FAILED\r\n");
        return;
    }
    LOG_Printf("[SRAM] ExtMem pool ready: %uKB @0x%08X (LVGL backend)\r\n",
               (unsigned)(MEM_EXT_POOL_SIZE / 1024u),
               (unsigned)MEM_EXT_POOL_BASE);
}
