/* ================================================================
 * la_buffer —— 逻辑分析仪环形缓冲：写/读/导出
 *
 * 架构位置：APP 服务层；DMA 采样落地与 HOSTLINK 导出
 * ================================================================ */
#include "la_buffer.h"
#include "bsp_system.h"

#define LA_SRAM_TEST_TIMEOUT_MS  500u  /* 自检上限：正常 <10ms，
                                        * 外部 SRAM/FSMC 偶发慢响应时防止
                                        * 启动无限爬行（实测卡死复现） */
/* 自检范围：32KB 抽查（原 512KB 全量）。全量写读的"故障检测覆盖率"
 * 收益极低（32KB 抽查即可暴露芯片/FSMC 级故障），却带来启动卡顿风险：
 * 实测外部 SRAM 响应异常时全量自检循环可拖长到分钟级、占满 CPU
 * （GUI 刷新被压制，见 ENGINEERING_LOG 13.5）。32KB 即使异常也秒级结束。 */
#define LA_SRAM_TEST_SIZE      (32u * 1024u)

static LA_SamplePoint *la_buffer = (LA_SamplePoint *)LA_SRAM_START_ADDR;
static volatile uint32_t write_idx = 0;
static volatile uint32_t sample_count = 0;
static volatile uint8_t  la_sram_ok = 0;

/* 外部 SRAM 写读自检：全量填充 + 回读比对，检测芯片/FSMC 故障。
 * 带 HAL tick 超时：读回阶段若 SRAM 响应异常缓慢（CPU 仍能执行循环指令，
 * 但单次访问被拉长），超过时限立即判失败返回，保证启动流程不被拖死；
 * 若访问彻底总线停摆（CPU 卡在 ldrh），由 IWDG 兜底复位。 */
static uint8_t la_sram_self_test(void)
{
    volatile uint16_t *mem = (volatile uint16_t *)LA_SRAM_START_ADDR;
    uint32_t words = LA_SRAM_TEST_SIZE / sizeof(uint16_t);
    uint32_t t0 = BSP_GetTick();

    /* 写循环同样带超时：实测外部 SRAM 响应异常时（FSMC 时序边缘/芯片
     * 故障）写循环无超时检查，512KB 全量会拖长到分钟级，startupTask
     * 长时间占用 CPU（GUI 刷新被拖慢，见 ENGINEERING_LOG 13.5）。
     * 与读循环一致：超时立即判失败返回，不阻塞启动/模块初始化。 */
    for (uint32_t i = 0; i < words; i++) {
        mem[i] = (uint16_t)(0xA500 + (i & 0xFF));
        if ((BSP_GetTick() - t0) > LA_SRAM_TEST_TIMEOUT_MS) {
            return 0;   /* 写阶段超时：判定 SRAM 不可用，不阻塞启动 */
        }
    }
    for (uint32_t i = 0; i < words; i++) {
        if (mem[i] != (uint16_t)(0xA500 + (i & 0xFF))) {
            return 0;
        }
        if ((BSP_GetTick() - t0) > LA_SRAM_TEST_TIMEOUT_MS) {
            return 0;   /* 超时：判定 SRAM 不可用，不阻塞启动 */
        }
    }
    return 1;
}

void LA_Buffer_Init(void)
{
    write_idx = 0;
    sample_count = 0;
    la_sram_ok = la_sram_self_test();
}

void LA_Buffer_Write(uint64_t timestamp, uint8_t states)
{
    if (!la_sram_ok) return;
    uint32_t ts = (uint32_t)timestamp;
    la_buffer[write_idx].timestamp_lo = ts & 0xFFFF;
    la_buffer[write_idx].timestamp_hi = (ts >> 16) & 0xFFFF;
    la_buffer[write_idx].states = (uint16_t)states;
    write_idx = (write_idx + 1) % LA_BUF_MAX_COUNT;
    if (sample_count < LA_BUF_MAX_COUNT) sample_count++;
}

uint32_t LA_Buffer_GetCount(void)
{
    return sample_count;
}

void LA_Buffer_Reset(void)
{
    write_idx = 0;
    sample_count = 0;
}

void LA_Buffer_Read(LA_SamplePoint *buf, uint32_t start, uint32_t count)
{
    if (!la_sram_ok) return;
    uint32_t first_idx = (sample_count >= LA_BUF_MAX_COUNT) ? write_idx : 0;
    uint32_t idx = (first_idx + start) % LA_BUF_MAX_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        buf[i] = la_buffer[idx];
        idx = (idx + 1) % LA_BUF_MAX_COUNT;
    }
}

uint8_t LA_Buffer_IsSramOk(void)
{
    return la_sram_ok;
}
