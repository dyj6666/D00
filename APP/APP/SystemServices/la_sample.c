#include "la_sample.h"
#include "la_buffer.h"
#include "la_trigger.h"
#include "stm32f4xx_hal.h"
#include "logger.h"
#include "pinout.h"
#include "tim.h"
#include "var_manager.h"
#include "var_ids.h"

/* 采样状态变量（供 shell / 变量注册使用） */
uint32_t la_samples = 0;
uint32_t la_ch0_state = 0;
uint32_t la_ch4_state = 0;

void LA_RegisterVariables(void)
{
    VAR_Register(VAR_ID_LA_SAMPLES, "la_samples", VAR_TYPE_INT32, 0, &la_samples);
    VAR_Register(VAR_ID_LA_CH0,     "la_ch0",     VAR_TYPE_INT32, 0, &la_ch0_state);
    VAR_Register(VAR_ID_LA_CH4,     "la_ch4",     VAR_TYPE_INT32, 0, &la_ch4_state);
}

/* --------------------- 时间戳模式（EXTI）相关 --------------------- */
#define PRE_TRIGGER_DEPTH 1024
#define POST_TRIGGER_SAMPLES 2048   /* 触发后继续采样的点数，之后自动停采 */

static LA_SamplePoint pre_trigger_buf[PRE_TRIGGER_DEPTH];
static volatile uint32_t ts_overflow = 0;
static volatile LA_SampleMode current_mode = LA_MODE_IDLE;
static volatile uint32_t pre_trigger_idx = 0;
static volatile uint32_t pre_trigger_count = 0;
static volatile uint32_t post_trigger_count = 0;
static volatile uint8_t  trigger_fired = 0;
static volatile uint8_t  capture_done = 0;
static uint8_t last_trigger_state = 0xFF;   /* 触发通道上一状态，用于边沿检测 */

/* --------------------- DMA 流模式相关 ---------------------
 * 引擎：TIM1 更新事件 -> DMA2_Stream5(Ch6) 将 GPIOB->IDR 整字搬入环形缓冲。
 * TIM1 位于 APB2(168MHz)，采样率由 PSC/ARR 组合精确设定。
 * DMA1 无法访问 AHB1（GPIO），因此不能用 TIM3+DMA1 做此功能。 */
#define LA_TIM_CLOCK_HZ  168000000UL
static uint32_t la_stream_buf[LA_DMA_BUF_SIZE] __attribute__((aligned(4)));
static volatile uint32_t dma_transfer_count = 0;  /* 满传输（8192点）完成次数 */
static volatile uint32_t dma_completed = 0;       /* 停止时固化样本总数 */
static volatile uint8_t  dma_running = 0;

/* DMA 半/全传输回调（ISR） */
static void la_dma_half_cb(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
}

static void la_dma_full_cb(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    if (dma_running) {
        dma_transfer_count++;
    }
}

/* ================================================================
 *                    通用初始化 & 时间戳模式
 * ================================================================ */

void LA_Sample_Init(void)
{
    LA_RegisterVariables();
    HAL_TIM_Base_Start(&htim2);
    __HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE);
    ts_overflow = 0;

    /* 注册 DMA 回调（ISR 上下文调用） */
    HAL_DMA_RegisterCallback(&hdma_tim1_up, HAL_DMA_XFER_HALFCPLT_CB_ID, la_dma_half_cb);
    HAL_DMA_RegisterCallback(&hdma_tim1_up, HAL_DMA_XFER_CPLT_CB_ID, la_dma_full_cb);
}

void LA_Sample_Start(LA_SampleMode mode)
{
    if (mode == LA_MODE_TIMESTAMP) {
        if (!LA_Buffer_IsSramOk()) {
            LOG_Printf("LA: SRAM self-test failed, capture aborted\r\n");
            return;
        }
        __HAL_TIM_SET_COUNTER(&htim2, 0);
        ts_overflow = 0;
        LA_Buffer_Reset();
        trigger_fired = 0;
        capture_done = 0;
        pre_trigger_idx = 0;
        pre_trigger_count = 0;
        post_trigger_count = 0;
        last_trigger_state = 0xFF;

        if (LA_Trigger_GetType() == LA_TRIG_NONE) {
            /* 无触发：直接进入连续采集 */
            trigger_fired = 1;
            capture_done = 1;
        } else {
            LA_Trigger_Arm();
        }

        HAL_NVIC_EnableIRQ(EXTI0_IRQn);
        HAL_NVIC_EnableIRQ(EXTI1_IRQn);
        HAL_NVIC_EnableIRQ(EXTI2_IRQn);
        HAL_NVIC_EnableIRQ(EXTI3_IRQn);
        HAL_NVIC_EnableIRQ(EXTI4_IRQn);
        HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

        current_mode = LA_MODE_TIMESTAMP;
    }
}

void LA_Diag_PrintExtiStatus(void)
{
    LOG_Printf("EXTI4 IMR: %s\r\n",
               (EXTI->IMR & EXTI_IMR_IM4) ? "enabled" : "DISABLED");
    LOG_Printf("EXTI4 IRQ: %s\r\n",
               (NVIC->ISER[EXTI4_IRQn >> 5] & (1 << (EXTI4_IRQn & 0x1F))) ?
               "enabled" : "DISABLED");
}

void LA_Sample_Stop(void)
{
    if (current_mode == LA_MODE_TIMESTAMP) {
        HAL_NVIC_DisableIRQ(EXTI0_IRQn);
        HAL_NVIC_DisableIRQ(EXTI1_IRQn);
        HAL_NVIC_DisableIRQ(EXTI2_IRQn);
        HAL_NVIC_DisableIRQ(EXTI3_IRQn);
        HAL_NVIC_DisableIRQ(EXTI4_IRQn);
        HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        current_mode = LA_MODE_IDLE;

        /* 触发后停止：把预触发环形缓冲按时间顺序补到 SRAM 缓冲前部
         * （自动停采 capture_done 时同样需要补） */
        if (trigger_fired) {
            uint32_t i, idx;
            idx = (pre_trigger_idx - pre_trigger_count + PRE_TRIGGER_DEPTH) % PRE_TRIGGER_DEPTH;
            for (i = 0; i < pre_trigger_count; i++) {
                LA_Buffer_Write(((uint64_t)pre_trigger_buf[idx].timestamp_hi << 32) |
                                ((uint64_t)pre_trigger_buf[idx].timestamp_lo),
                                (uint8_t)pre_trigger_buf[idx].states);
                idx = (idx + 1) % PRE_TRIGGER_DEPTH;
            }
        }
        la_samples = LA_Buffer_GetCount();
        LA_Trigger_Reset();
    }
}

uint64_t LA_Sample_GetTimestamp(void)
{
    uint32_t cnt = __HAL_TIM_GET_COUNTER(&htim2);
    uint32_t ovf = ts_overflow;
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE)) {
        cnt = __HAL_TIM_GET_COUNTER(&htim2);
        ovf = ts_overflow;
    }
    return ((uint64_t)ovf << 32) | cnt;
}

uint8_t LA_Sample_GetChannelStates(void)
{
    return (uint8_t)(GPIOB->IDR & 0xFF);
}

void LA_Timestamp_Overflow_Handler(void)
{
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE)) {
        __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
        ts_overflow++;
    }
}

static uint8_t la_pin_to_channel(uint16_t pin)
{
    switch (pin) {
        case GPIO_PIN_0: return 0;
        case GPIO_PIN_1: return 1;
        case GPIO_PIN_2: return 2;
        case GPIO_PIN_3: return 3;
        case GPIO_PIN_4: return 4;
        case GPIO_PIN_5: return 5;
        case GPIO_PIN_6: return 6;
        case GPIO_PIN_7: return 7;
        default:         return 0xFF;
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (current_mode != LA_MODE_TIMESTAMP) return;
    if (GPIO_Pin < GPIO_PIN_0 || GPIO_Pin > GPIO_PIN_7) return;

    uint64_t timestamp = LA_Sample_GetTimestamp();
    uint8_t states = LA_Sample_GetChannelStates();

    if (!trigger_fired) {
        /* 预触发缓冲 */
        pre_trigger_buf[pre_trigger_idx].timestamp_lo = (uint32_t)timestamp & 0xFFFF;
        pre_trigger_buf[pre_trigger_idx].timestamp_hi = (uint32_t)(timestamp >> 16) & 0xFFFF;
        pre_trigger_buf[pre_trigger_idx].states = (uint16_t)states;

        pre_trigger_idx = (pre_trigger_idx + 1) % PRE_TRIGGER_DEPTH;
        if (pre_trigger_count < PRE_TRIGGER_DEPTH) {
            pre_trigger_count++;
        }

        /* 触发检测（上升/下降/任意边沿） */
        uint8_t channel = la_pin_to_channel(GPIO_Pin);
        if (LA_Trigger_IsArmed() && channel == LA_Trigger_GetChannel()) {
            uint8_t current_state = (states & (1 << channel)) ? 1 : 0;
            uint8_t prev_state = last_trigger_state;
            LA_TriggerType type = LA_Trigger_GetType();

            if ((type == LA_TRIG_EDGE_RISING  && current_state == 1 && prev_state == 0) ||
                (type == LA_TRIG_EDGE_FALLING && current_state == 0 && prev_state == 1) ||
                (type == LA_TRIG_EDGE_ANY     && current_state != prev_state)) {
                trigger_fired = 1;
                post_trigger_count = 0;
                LA_Trigger_SetTriggered();
            }
            last_trigger_state = current_state;
        }
    } else {
        /* 触发后直接写入外部 SRAM */
        LA_Buffer_Write(timestamp, states);
        post_trigger_count++;
        if (post_trigger_count >= POST_TRIGGER_SAMPLES) {
            /* 触发深度满足，自动停采，保留缓冲数据 */
            capture_done = 1;
            HAL_NVIC_DisableIRQ(EXTI0_IRQn);
            HAL_NVIC_DisableIRQ(EXTI1_IRQn);
            HAL_NVIC_DisableIRQ(EXTI2_IRQn);
            HAL_NVIC_DisableIRQ(EXTI3_IRQn);
            HAL_NVIC_DisableIRQ(EXTI4_IRQn);
            HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        }
    }

    /* 状态变量（调试/上位机读取） */
    la_samples = LA_Buffer_GetCount();
    la_ch0_state = (states & 0x01) ? 1 : 0;
    la_ch4_state = (states & 0x10) ? 1 : 0;
}

/* ================================================================
 *             DMA 流模式（TIM1 + DMA2，真硬件采样）
 * ================================================================ */

void LA_Sample_Start_DMA(uint32_t sample_rate_hz)
{
    if (current_mode != LA_MODE_IDLE) {
        LOG_Printf("LA: busy (mode=%u)\r\n", (unsigned)current_mode);
        return;
    }
    if (sample_rate_hz == 0) sample_rate_hz = 1000;

    /* 在 16 位 PSC/ARR 范围内寻找最接近的整型分频组合 */
    uint32_t psc = 0, arr = 0;
    int found = 0;
    for (psc = 0; psc <= 65535; psc++) {
        uint32_t div = LA_TIM_CLOCK_HZ / (psc + 1);
        if (div == 0) break;
        uint32_t p = div / sample_rate_hz;
        if (p >= 1 && p <= 65535) {
            arr = p - 1;
            found = 1;
            break;
        }
    }
    if (!found) {
        LOG_Printf("LA: rate %lu Hz unsupported\r\n", (unsigned long)sample_rate_hz);
        return;
    }

    __HAL_TIM_SET_PRESCALER(&htim1, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
    __HAL_TIM_SET_COUNTER(&htim1, 0);

    dma_transfer_count = 0;
    dma_completed = 0;
    dma_running = 0;
    current_mode = LA_MODE_DMA_STREAM;

    /* 外围地址固定为 GPIOB->IDR（AHB1，仅 DMA2 可访问） */
    if (HAL_DMA_Start_IT(&hdma_tim1_up, (uint32_t)&GPIOB->IDR,
                         (uint32_t)la_stream_buf, LA_DMA_BUF_SIZE) != HAL_OK) {
        current_mode = LA_MODE_IDLE;
        LOG_Printf("LA: DMA start failed\r\n");
        return;
    }
    dma_running = 1;   /* 先置位，避免首个满传输回调被漏计 */
    __HAL_TIM_ENABLE_DMA(&htim1, TIM_DMA_UPDATE);
    if (HAL_TIM_Base_Start(&htim1) != HAL_OK) {
        dma_running = 0;
        __HAL_TIM_DISABLE_DMA(&htim1, TIM_DMA_UPDATE);
        HAL_DMA_Abort(&hdma_tim1_up);
        current_mode = LA_MODE_IDLE;
        LOG_Printf("LA: TIM1 start failed\r\n");
        return;
    }

    LOG_Printf("LA: DMA capture started, rate=%lu Hz (psc=%lu arr=%lu)\r\n",
               (unsigned long)sample_rate_hz, (unsigned long)psc, (unsigned long)arr);
}

uint32_t LA_Sample_Stop_DMA(void)
{
    if (current_mode != LA_MODE_DMA_STREAM) {
        return dma_completed;
    }

    /* 先取有效计数，再停硬件 */
    uint32_t count = LA_Sample_GetDMACount();
    dma_running = 0;
    __HAL_TIM_DISABLE_DMA(&htim1, TIM_DMA_UPDATE);
    HAL_TIM_Base_Stop(&htim1);
    HAL_DMA_Abort(&hdma_tim1_up);

    current_mode = LA_MODE_IDLE;
    dma_completed = count;
    la_samples = count;
    return count;
}

uint32_t LA_Sample_GetDMACount(void)
{
    if (dma_running) {
        uint32_t rem = __HAL_DMA_GET_COUNTER(&hdma_tim1_up);
        if (rem > LA_DMA_BUF_SIZE) rem = LA_DMA_BUF_SIZE;
        return dma_transfer_count * LA_DMA_BUF_SIZE + (LA_DMA_BUF_SIZE - rem);
    }
    return dma_completed;
}

void LA_Sample_ReadDMABuffer(uint32_t *buf, uint32_t start, uint32_t count)
{
    if (buf == NULL) return;
    for (uint32_t i = 0; i < count; i++) {
        buf[i] = la_stream_buf[(start + i) % LA_DMA_BUF_SIZE];
    }
}
