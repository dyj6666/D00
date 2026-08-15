/* ================================================================
 * la_config —— 逻辑分析仪配置：采样率/通道/深度
 *
 * 架构位置：APP 配置层；采样相关常量单一来源
 * ================================================================ */
#ifndef LA_CONFIG_H
#define LA_CONFIG_H

#include <stdint.h>
#include "stm32f4xx.h"
#include "mem_map.h"

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

/* ---------------- DMA 流模式缓冲（统一外部 SRAM） ----------------
 * 外部 SRAM 32768 点（128 KB，FSMC NE3）；DMA 吞吐实测约 6 MHz 上限。
 * 原 32KB 内部 IRAM 缓冲已移除（RAM 瘦身：内部 SRAM 让给堆/任务/LwIP）。
 * DMA 模式与时间戳模式互斥，共用同一片外部 SRAM。 */
#define LA_DMA_SRAM_SIZE      32768
#define LA_DMA_SRAM_ADDR      LA_SRAM_START_ADDR

/* ---------------- 采样通道映射（改这里即可换引脚） ----------------
 * LA_GPIO_PORT      采样端口（DMA 单端口读取）
 * LA_CHANNEL_PINS   通道 0..7 对应的引脚，0 表示该通道未使用；
 *                   导出时自动归一化为 数据位 i = 通道 i（上位机无需感知）。
 * 当前 4 通道：探索者V3 的 PG6/PG7/PG8/PG15（全部“完全独立”）。
 */
#define LA_GPIO_PORT         GPIOG
#define LA_CHANNEL_PINS      {GPIO_PIN_6, GPIO_PIN_7, GPIO_PIN_8, GPIO_PIN_15, \
                               0, 0, 0, 0}

/* ---------------- 时间戳模式 ---------------- */
#define PRE_TRIGGER_DEPTH    1024    /* 预触发环形缓冲深度（内部 RAM） */
#define LA_MAX_CHANNELS      8
#define LA_TIMESTAMP_TIMER   TIM2

/* 采样点结构：3 个 16 位字，适配外部 16 位 SRAM */
typedef struct {
    uint16_t timestamp_lo;
    uint16_t timestamp_hi;
    uint16_t states;
} LA_SamplePoint;

#define LA_SRAM_START_ADDR   MEM_LA_BASE          /* 见 Config/mem_map.h 单一事实源 */
#define LA_BUFFER_SIZE       MEM_LA_AREA_SIZE
#define LA_BUF_MAX_COUNT     (LA_BUFFER_SIZE / sizeof(LA_SamplePoint))

#endif
