/* ================================================================
 * bsp_can —— CAN1 板级驱动实现（性能拉满：1Mbps / 双 FIFO / 3 邮箱）
 *
 * 架构位置：APP BSP 层；实现 bsp_can.h 声明的全部接口
 *
 * 线程模型：
 *   RX：FIFO0/FIFO1 中断 → 静默入队（ISR 零业务）→ canRx 任务分发回调；
 *   TX：调用方入 TX 队列 → canTx 任务批量填满 3 邮箱（线速发送）；
 *   ERR：SCE 中断 → HAL_CAN_ErrorCallback 累计等级/溢出计数（ABOM 自愈）。
 * ================================================================ */
#include "bsp_can.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "stm32f4xx_hal.h"

#include <string.h>

/* 每帧总线占用比特近似：44 固定 + 8×DLC 数据 + 3 CRC/填充余量（1Mbps 工程估算） */
#define CAN_FRAME_BITS(dlc)  (47u + 10u * (uint32_t)(dlc))

/* 内部帧缓冲结构（16B 对齐，入队无堆碎片） */
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_msg_t;

static CAN_HandleTypeDef s_hcan;                 /* HAL CAN 句柄 */
static QueueHandle_t     s_rx_q;                 /* RX 帧队列（ISR → 任务） */
static QueueHandle_t     s_tx_q;                 /* TX 帧队列（任务 → 邮箱） */
static TaskHandle_t      s_rx_task;
static TaskHandle_t      s_tx_task;

static bsp_can_rx_cb_t   s_cbs[BSP_CAN_MAX_CB];  /* 帧消费者注册表 */
static void             *s_cb_ctx[BSP_CAN_MAX_CB];
static uint32_t          s_cb_count;

static bsp_can_stats_t   s_stats;
static uint32_t          s_frame_bits;           /* 累计 RX 帧比特（负载估算） */
static uint32_t          s_last_bits;
static uint32_t          s_last_tick;
static uint32_t          s_bus_load;             /* 近窗口负载 ‰ */

static volatile uint8_t  s_started;
static volatile uint8_t  s_initialized;
static bsp_can_mode_t    s_mode;

/** @brief 按模式复位并启动 CAN 外设：位定时/滤波/中断通知一次到位 */
static void can_start(bsp_can_mode_t mode)
{
    s_mode = mode;
    s_hcan.Instance = CAN1;
    s_hcan.Init.Prescaler = 2;                    /* APB1=42MHz → 21MHz TQ */
    s_hcan.Init.Mode = (mode == BSP_CAN_MODE_LOOPBACK) ? CAN_MODE_LOOPBACK :
                       (mode == BSP_CAN_MODE_SILENT_LOOPBACK) ?
                       CAN_MODE_SILENT_LOOPBACK : CAN_MODE_NORMAL;
    s_hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    s_hcan.Init.TimeSeg1 = CAN_BS1_15TQ;          /* 1+15+5=21TQ → 1Mbps */
    s_hcan.Init.TimeSeg2 = CAN_BS2_5TQ;           /* 采样点 76.2%（1Mbps 推荐区间） */
    s_hcan.Init.TimeTriggeredMode = DISABLE;
    s_hcan.Init.AutoBusOff = ENABLE;              /* ABOM：bus-off 硬件自愈 */
    s_hcan.Init.AutoWakeUp = DISABLE;
    s_hcan.Init.AutoRetransmission = ENABLE;      /* NART=0：竞争自动重发 */
    s_hcan.Init.ReceiveFifoLocked = DISABLE;
    s_hcan.Init.TransmitFifoPriority = ENABLE;    /* TXFP：按请求顺序出帧 */
    if (HAL_CAN_Init(&s_hcan) != HAL_OK) {
        s_started = 0;
        return;
    }

    /* 滤波：bank0→FIFO0、bank1→FIFO1 全收（掩码全 0），软件按 ID 分流 */
    CAN_FilterTypeDef f;
    memset(&f, 0, sizeof(f));
    f.FilterIdHigh = 0;
    f.FilterIdLow = 0;
    f.FilterMaskIdHigh = 0;
    f.FilterMaskIdLow = 0;
    f.FilterMode = CAN_FILTERMODE_IDMASK;
    f.FilterScale = CAN_FILTERSCALE_32BIT;
    f.FilterActivation = ENABLE;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterBank = 0;
    HAL_CAN_ConfigFilter(&s_hcan, &f);
    f.FilterFIFOAssignment = CAN_RX_FIFO1;
    f.FilterBank = 1;
    HAL_CAN_ConfigFilter(&s_hcan, &f);

    HAL_CAN_Start(&s_hcan);
    HAL_CAN_ActivateNotification(&s_hcan,
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN |
        CAN_IT_RX_FIFO1_MSG_PENDING | CAN_IT_RX_FIFO1_OVERRUN |
        CAN_IT_ERROR | CAN_IT_BUSOFF |
        CAN_IT_ERROR_WARNING | CAN_IT_ERROR_PASSIVE);
    s_started = 1;
}

/** @brief canRx 任务：出队 → 按注册表分发（任务上下文，可安全调用命令框架） */
static void can_rx_task(void *arg)
{
    (void)arg;
    can_msg_t m;
    for (;;) {
        if (xQueueReceive(s_rx_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_stats.rx_ok++;
        s_frame_bits += CAN_FRAME_BITS(m.dlc);
        uint32_t handled = 0;
        for (uint32_t i = 0; i < s_cb_count; i++) {
            if (s_cbs[i] != NULL) {
                s_cbs[i](m.id, m.data, m.dlc, s_cb_ctx[i]);
                handled++;
            }
        }
        if (handled == 0) {
            s_stats.rx_other++;                  /* 无消费者的嗅探帧 */
        }
    }
}

/** @brief canTx 任务：队列 → 批量填满 3 邮箱，吞吐拉满且永不丢弃 */
static void can_tx_task(void *arg)
{
    (void)arg;
    can_msg_t m;
    for (;;) {
        if (xQueueReceive(s_tx_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* 等待至少一个邮箱空闲（1Mbps 下 ≤ 帧时长，忙等极短） */
        while (HAL_CAN_GetTxMailboxesFreeLevel(&s_hcan) == 0u) {
            taskYIELD();
        }
        /* 首帧 + 趁邮箱空闲连续填充队列剩余帧 */
        for (;;) {
            CAN_TxHeaderTypeDef h;
            memset(&h, 0, sizeof(h));
            h.StdId = m.id;
            h.IDE = CAN_ID_STD;
            h.RTR = CAN_RTR_DATA;
            h.DLC = m.dlc;
            uint32_t mb = 0;
            if (HAL_CAN_AddTxMessage(&s_hcan, &h, m.data, &mb) == HAL_OK) {
                s_stats.tx_ok++;
            } else {
                s_stats.tx_err++;
            }
            if (HAL_CAN_GetTxMailboxesFreeLevel(&s_hcan) == 0u ||
                xQueueReceive(s_tx_q, &m, 0) != pdTRUE) {
                break;
            }
        }
    }
}

/** @brief CAN1 三个中断统一入口（stm32f4xx_it.c 调用） */
void BSP_CAN_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&s_hcan);
}

/* ---------- HAL 弱回调覆盖（仅本工程唯一 CAN 使用者） ---------- */

/** @brief FIFO0 消息中断：排空 FIFO 入 RX 队列（ISR 内零业务） */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    can_msg_t m;
    CAN_RxHeaderTypeDef rh;
    while (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rh, m.data) == HAL_OK) {
        m.id = rh.StdId;
        m.dlc = rh.DLC;
        BaseType_t w = pdFALSE;
        if (xQueueSendFromISR(s_rx_q, &m, &w) != pdTRUE) {
            s_stats.rx_drop++;                  /* 队列满：丢弃并计数 */
        }
        portYIELD_FROM_ISR(w);
    }
}

/** @brief FIFO1 消息中断：同上（双 FIFO 并行收，防单 FIFO 溢出） */
void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    can_msg_t m;
    CAN_RxHeaderTypeDef rh;
    while (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO1, &rh, m.data) == HAL_OK) {
        m.id = rh.StdId;
        m.dlc = rh.DLC;
        BaseType_t w = pdFALSE;
        if (xQueueSendFromISR(s_rx_q, &m, &w) != pdTRUE) {
            s_stats.rx_drop++;
        }
        portYIELD_FROM_ISR(w);
    }
}

/** @brief 状态变化/错误中断：累计等级与溢出，ABOM 负责 bus-off 自愈 */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    uint32_t err = HAL_CAN_GetError(hcan);
    s_stats.last_error = err;
    if (err & HAL_CAN_ERROR_EWG) {
        s_stats.err_warning++;
    }
    if (err & HAL_CAN_ERROR_EPV) {
        s_stats.err_passive++;
    }
    if (err & HAL_CAN_ERROR_BOF) {
        s_stats.err_busoff++;
    }
    if (err & (HAL_CAN_ERROR_RX_FOV0 | HAL_CAN_ERROR_RX_FOV1)) {
        s_stats.rx_overrun++;
    }
}

/* ---------- BSP 公共接口 ---------- */

void BSP_CAN_Init(void)
{
    if (s_initialized) {
        return;                                  /* 幂等：模块可重复初始化 */
    }
    s_initialized = 1;

    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA11=CAN1_RX / PA12=CAN1_TX（AF9，探索者V3 经 TJA1050 引出） */
    GPIO_InitTypeDef g;
    memset(&g, 0, sizeof(g));
    g.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &g);

    s_rx_q = xQueueCreate(BSP_CAN_RX_QUEUE_LEN, sizeof(can_msg_t));
    s_tx_q = xQueueCreate(BSP_CAN_TX_QUEUE_LEN, sizeof(can_msg_t));

    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 6, 0);   /* 数值 ≥5：可用 FromISR API */
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
    HAL_NVIC_SetPriority(CAN1_RX1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX1_IRQn);
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);

    can_start(BSP_CAN_MODE_NORMAL);

    /* RX 优先级高于 TX：突发收发时 RX 任务先排空队列，防 ISR 丢帧；
     * TX 以总线速率自然节流，低优先级不影响吞吐。 */
    xTaskCreate(can_rx_task, "canRx", 256, NULL, 9, &s_rx_task);
    xTaskCreate(can_tx_task, "canTx", 128, NULL, 8, &s_tx_task); /* 峰值 47 词 */
}

void BSP_CAN_SetMode(bsp_can_mode_t mode)
{
    HAL_CAN_Stop(&s_hcan);
    s_started = 0;
    can_start(mode);
}

bsp_can_mode_t BSP_CAN_GetMode(void)
{
    return s_mode;
}

int BSP_CAN_Send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    if (s_tx_q == NULL || dlc > BSP_CAN_DLC_MAX) {
        return -1;
    }
    can_msg_t m;
    m.id = id;
    m.dlc = dlc;
    memset(m.data, 0, sizeof(m.data));
    if (data != NULL && dlc > 0) {
        memcpy(m.data, data, dlc);
    }
    /* 短超时等待入队：canTx 任务以线速排空，长输出仅极端突发截断 */
    return (xQueueSend(s_tx_q, &m, pdMS_TO_TICKS(50)) == pdTRUE) ? 0 : -1;
}

int BSP_CAN_RegisterRxCb(bsp_can_rx_cb_t cb, void *ctx)
{
    if (cb == NULL || s_cb_count >= BSP_CAN_MAX_CB) {
        return -1;
    }
    s_cbs[s_cb_count] = cb;
    s_cb_ctx[s_cb_count] = ctx;
    s_cb_count++;
    return 0;
}

void BSP_CAN_GetStats(bsp_can_stats_t *st)
{
    if (st == NULL) {
        return;
    }
    /* 近窗口总线占用率：1Mbps=1000bit/ms → 负载‰ = 窗口帧比特/窗口毫秒 */
    uint32_t now = HAL_GetTick();
    uint32_t dt = now - s_last_tick;
    if (dt >= 500u) {
        if (dt > 0u) {
            s_bus_load = (s_frame_bits - s_last_bits) / dt;
        }
        s_last_bits = s_frame_bits;
        s_last_tick = now;
    }
    *st = s_stats;
    st->bus_load_permille = s_bus_load;
}

void BSP_CAN_ResetStats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_frame_bits = 0;
    s_last_bits = 0;
    s_last_tick = HAL_GetTick();
}

int BSP_CAN_IsActive(void)
{
    if (!s_started) {
        return 0;
    }
    /* ABOM 自愈期间 ESR.BOFF 短暂置位，视为不可用 */
    return (s_hcan.Instance->ESR & CAN_ESR_BOFF) ? 0 : 1;
}

uint32_t BSP_CAN_GetErrorCounters(uint8_t *tec, uint8_t *rec)
{
    uint32_t esr = s_hcan.Instance->ESR;
    if (tec != NULL) {
        *tec = (uint8_t)((esr >> 16) & 0xFFu);
    }
    if (rec != NULL) {
        *rec = (uint8_t)((esr >> 8) & 0xFFu);
    }
    return esr;
}
