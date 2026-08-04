/* 逻辑分析仪硬件与缓冲配置 */
#ifndef LA_CONFIG_H
#define LA_CONFIG_H

#include <stdint.h>

typedef enum {
    LA_MODE_IDLE = 0,
    LA_MODE_TIMESTAMP,
    LA_MODE_DMA_STREAM
} LA_SampleMode;

/* ---------------- 触发配置 ---------------- */
typedef enum {
    LA_TRIG_NONE = 0,
    LA_TRIG_EDGE_RISING,
    LA_TRIG_EDGE_FALLING,
    LA_TRIG_EDGE_ANY
} LA_TriggerType;

typedef struct {
    LA_TriggerType type;
    uint8_t  channel;       /* 边沿触发通道（0~7） */
    uint16_t post_samples;  /* 触发后继续采集点数（自动停采阈值） */
    uint8_t  cond_channel;  /* 条件通道（0xFF = 无条件，单通道边沿触发） */
    uint8_t  cond_level;    /* 条件通道期望电平（0/1） */
} la_trigger_cfg_t;

/* ---------------- DMA 流模式缓冲（运行时可选） ----------------
 * IRAM：内部 RAM 8192 点（32 KB），DMA 吞吐高（实测 ≥21 MHz）；
 * SRAM：外部 SRAM 32768 点（128 KB，深度 4 倍），受 FSMC 带宽限制
 *       （实测约 6 MHz 上限）。默认 SRAM，可用 la_dma_buf 切换。
 * DMA 模式与时间戳模式互斥，共用同一片外部 SRAM。 */
#define LA_DMA_IRAM_SIZE      8192
#define LA_DMA_SRAM_SIZE      32768
#define LA_DMA_SRAM_ADDR      LA_SRAM_START_ADDR

/* ---------------- 时间戳模式 ---------------- */
#define PRE_TRIGGER_DEPTH    1024    /* 预触发环形缓冲深度（内部 RAM） */
#define LA_MAX_CHANNELS      8
#define LA_TIMESTAMP_TIMER   TIM2
#define LA_GPIO_PORT         GPIOB
#define LA_GPIO_PIN_MASK     0x00FF

/* 采样点结构：3 个 16 位字，适配外部 16 位 SRAM */
typedef struct {
    uint16_t timestamp_lo;
    uint16_t timestamp_hi;
    uint16_t states;
} LA_SamplePoint;

#define LA_SRAM_START_ADDR   0x68000000
#define LA_BUFFER_SIZE       (512 * 1024)
#define LA_BUF_MAX_COUNT     (LA_BUFFER_SIZE / sizeof(LA_SamplePoint))

#endif
