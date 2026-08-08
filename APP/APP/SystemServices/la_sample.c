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
uint32_t la_ch3_state = 0;

void LA_RegisterVariables(void)
{
    VAR_Register(VAR_ID_LA_SAMPLES, "la_samples", VAR_TYPE_INT32, 0, &la_samples);
    VAR_Register(VAR_ID_LA_CH0,     "la_ch0",     VAR_TYPE_INT32, 0, &la_ch0_state);
    VAR_Register(VAR_ID_LA_CH3,     "la_ch3",     VAR_TYPE_INT32, 0, &la_ch3_state);
}

/* --------------------- 时间戳模式（EXTI）相关 --------------------- */
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
 * 引擎：TIM1 更新事件 -> DMA2_Stream5(Ch6) 将 LA_GPIO_PORT->IDR 整字搬入环形缓冲。
 * TIM1 位于 APB2(168MHz)，采样率由 PSC/ARR 组合精确设定。
 * DMA1 无法访问 AHB1（GPIO），因此不能用 TIM3+DMA1 做此功能。 */
#define LA_TIM_CLOCK_HZ  168000000UL
/* 通道→引脚映射（LA_CHANNEL_PINS），0 = 通道未使用 */
static const uint16_t la_ch_pins[LA_MAX_CHANNELS] = LA_CHANNEL_PINS;
static uint32_t la_stream_iram[LA_DMA_IRAM_SIZE] __attribute__((aligned(4)));
static uint32_t *la_stream_buf = (uint32_t *)LA_DMA_SRAM_ADDR;
static uint32_t la_dma_buf_size = LA_DMA_SRAM_SIZE;
static volatile uint32_t dma_transfer_count = 0;  /* 满传输（8192点）完成次数 */
static volatile uint32_t dma_completed = 0;       /* 停止时固化样本总数 */
static volatile uint8_t  dma_running = 0;
static volatile uint8_t  dma_overrun = 0;         /* DMA 溢出/错误标志 */

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

static void la_dma_error_cb(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    dma_overrun = 1;
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
    HAL_DMA_RegisterCallback(&hdma_tim1_up, HAL_DMA_XFER_ERROR_CB_ID, la_dma_error_cb);

    LOG_Printf("[APP] LA   : DMA buffer = %s (%u points)\r\n",
               la_stream_buf == la_stream_iram ? "IRAM" : "SRAM",
               (unsigned)la_dma_buf_size);
}

int LA_Sample_SetDMABuffer(uint8_t use_sram)
{
    if (current_mode != LA_MODE_IDLE) {
        return -2;      /* 采集中不允许切换缓冲 */
    }
    if (use_sram) {
        if (!LA_Buffer_IsSramOk()) return -1;
        la_stream_buf = (uint32_t *)LA_DMA_SRAM_ADDR;
        la_dma_buf_size = LA_DMA_SRAM_SIZE;
    } else {
        la_stream_buf = la_stream_iram;
        la_dma_buf_size = LA_DMA_IRAM_SIZE;
    }
    LOG_Printf("[APP] LA   : DMA buffer = %s (%u points)\r\n",
               use_sram ? "SRAM" : "IRAM", (unsigned)la_dma_buf_size);
    return 0;
}

uint8_t LA_Sample_IsDMASram(void)
{
    return (la_stream_buf != la_stream_iram);
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

        HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
        HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

        current_mode = LA_MODE_TIMESTAMP;
    }
}

void LA_Diag_PrintExtiStatus(void)
{
    uint32_t imr = EXTI->IMR;
    uint32_t nvic95 = NVIC->ISER[EXTI9_5_IRQn >> 5] & (1u << (EXTI9_5_IRQn & 0x1F));
    uint32_t nvic1510 = NVIC->ISER[EXTI15_10_IRQn >> 5] & (1u << (EXTI15_10_IRQn & 0x1F));
    LOG_Printf("EXTI IMR: 6=%s 7=%s 12=%s 15=%s\r\n",
               (imr & EXTI_IMR_IM6) ? "en" : "--",
               (imr & EXTI_IMR_IM7) ? "en" : "--",
               (imr & EXTI_IMR_IM12) ? "en" : "--",
               (imr & EXTI_IMR_IM15) ? "en" : "--");
    LOG_Printf("NVIC: EXTI9_5=%s EXTI15_10=%s\r\n",
               nvic95 ? "enabled" : "DISABLED",
               nvic1510 ? "enabled" : "DISABLED");
}

void LA_Sample_Stop(void)
{
    if (current_mode == LA_MODE_TIMESTAMP) {
        HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
        HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
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
    /* 打包：数据位 i = 通道 i 的电平（0 = 未使用通道恒 0） */
    uint16_t idr = (uint16_t)LA_GPIO_PORT->IDR;
    uint8_t states = 0;
    for (uint8_t i = 0; i < LA_MAX_CHANNELS; i++) {
        if (la_ch_pins[i] != 0 && (idr & la_ch_pins[i])) {
            states |= (uint8_t)(1u << i);
        }
    }
    return states;
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
    for (uint8_t i = 0; i < LA_MAX_CHANNELS; i++) {
        if (la_ch_pins[i] == pin) {
            return i;
        }
    }
    return 0xFF;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (current_mode != LA_MODE_TIMESTAMP) return;
    if (la_pin_to_channel(GPIO_Pin) == 0xFF) return;

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
            la_trigger_cfg_t cfg;
            LA_Trigger_GetConfig(&cfg);

            uint8_t edge_match =
                (type == LA_TRIG_EDGE_RISING  && current_state == 1 && prev_state == 0) ||
                (type == LA_TRIG_EDGE_FALLING && current_state == 0 && prev_state == 1) ||
                (type == LA_TRIG_EDGE_ANY     && current_state != prev_state);

            /* 条件触发：边沿发生时，条件通道电平必须匹配（如 I2C START） */
            uint8_t cond_match =
                (cfg.cond_channel == 0xFF) ||
                (((states >> cfg.cond_channel) & 1) == cfg.cond_level);

            if (edge_match && cond_match) {
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
        la_trigger_cfg_t cfg;
        LA_Trigger_GetConfig(&cfg);
        if (post_trigger_count >= cfg.post_samples) {
            /* 触发深度满足，自动停采，保留缓冲数据 */
            capture_done = 1;
            HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
            HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
        }
    }

    /* 状态变量（调试/上位机读取） */
    la_samples = LA_Buffer_GetCount();
    la_ch0_state = (states & 0x01) ? 1 : 0;
    la_ch3_state = (states & 0x08) ? 1 : 0;
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

    if (LA_Sample_IsDMASram() && !LA_Buffer_IsSramOk()) {
        LOG_Printf("LA: SRAM self-test failed, DMA capture aborted\r\n");
        return;
    }

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
    dma_overrun = 0;
    dma_running = 0;
    current_mode = LA_MODE_DMA_STREAM;

    /* 外围地址固定为 LA_GPIO_PORT->IDR（AHB1，仅 DMA2 可访问） */
    if (HAL_DMA_Start_IT(&hdma_tim1_up, (uint32_t)&LA_GPIO_PORT->IDR,
                         (uint32_t)la_stream_buf, la_dma_buf_size) != HAL_OK) {
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

    LOG_Printf("LA: DMA capture started, rate=%lu Hz (psc=%lu arr=%lu), buf=%u pts\r\n",
               (unsigned long)sample_rate_hz, (unsigned long)psc, (unsigned long)arr,
               (unsigned)la_dma_buf_size);
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
    if (dma_overrun) {
        LOG_Printf("LA: DMA overrun detected!\r\n");
    }
    return count;
}

uint8_t LA_Sample_GetDMAOverrun(void)
{
    return dma_overrun;
}

uint32_t LA_Sample_GetDMABufferSize(void)
{
    return la_dma_buf_size;
}

uint32_t LA_Sample_GetDMACount(void)
{
    if (dma_running) {
        uint32_t rem = __HAL_DMA_GET_COUNTER(&hdma_tim1_up);
        if (rem > la_dma_buf_size) rem = la_dma_buf_size;
        return dma_transfer_count * la_dma_buf_size + (la_dma_buf_size - rem);
    }
    return dma_completed;
}

void LA_Sample_ReadDMABuffer(uint32_t *buf, uint32_t start, uint32_t count)
{
    if (buf == NULL) return;
    for (uint32_t i = 0; i < count; i++) {
        /* 归一化：原始 IDR 字 → 通道位打包，保证数据位 i = 通道 i */
        uint32_t raw = la_stream_buf[(start + i) % la_dma_buf_size];
        uint32_t packed = 0;
        for (uint8_t c = 0; c < LA_MAX_CHANNELS; c++) {
            if (la_ch_pins[c] != 0 && (raw & la_ch_pins[c])) {
                packed |= (1u << c);
            }
        }
        buf[i] = packed;
    }
}
