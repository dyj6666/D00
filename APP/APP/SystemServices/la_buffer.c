/* 逻辑分析仪 SRAM 环形缓冲管理 */
#include "la_buffer.h"

static LA_SamplePoint *la_buffer = (LA_SamplePoint *)LA_SRAM_START_ADDR;
static volatile uint32_t write_idx = 0;
static volatile uint32_t sample_count = 0;
static volatile uint8_t  la_sram_ok = 0;

/* 外部 SRAM 写读自检：全量填充 + 回读比对，检测芯片/FSMC 故障 */
static uint8_t la_sram_self_test(void)
{
    volatile uint16_t *mem = (volatile uint16_t *)LA_SRAM_START_ADDR;
    uint32_t words = LA_BUFFER_SIZE / sizeof(uint16_t);

    for (uint32_t i = 0; i < words; i++) {
        mem[i] = (uint16_t)(0xA500 + (i & 0xFF));
    }
    for (uint32_t i = 0; i < words; i++) {
        if (mem[i] != (uint16_t)(0xA500 + (i & 0xFF))) {
            return 0;
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
