/* 逻辑分析仪硬件与缓冲配置 */
#ifndef LA_CONFIG_H
#define LA_CONFIG_H

#include <stdint.h>

typedef enum {
    LA_MODE_IDLE = 0,
    LA_MODE_TIMESTAMP,
    LA_MODE_DMA_STREAM
} LA_SampleMode;

/* DMA 流模式缓冲区 */
#define LA_DMA_BUF_SIZE    8192    // DMA缓冲区大小（采样点数）

typedef enum {
    LA_TRIG_NONE = 0,
    LA_TRIG_EDGE_RISING,
    LA_TRIG_EDGE_FALLING,
    LA_TRIG_EDGE_ANY
} LA_TriggerType;

#define LA_MAX_CHANNELS         8
#define LA_TIMESTAMP_TIMER      TIM2
#define LA_GPIO_PORT            GPIOB
#define LA_GPIO_PIN_MASK        0x00FF

/* 采样点结构：3 个 16 位字，适配外部 16 位 SRAM */
typedef struct {
    uint16_t timestamp_lo;
    uint16_t timestamp_hi;
    uint16_t states;
} LA_SamplePoint;

#define LA_SRAM_START_ADDR      0x68000000
#define LA_BUFFER_SIZE          (512 * 1024)
#define LA_BUF_MAX_COUNT        (LA_BUFFER_SIZE / sizeof(LA_SamplePoint))

#endif
