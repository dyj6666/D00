/* ================================================================
 * bsp_uart —— UART 抽象：DMA 收发/空闲断帧/回调分发
 *
 * 架构位置：APP BSP 层；logger/data_link 挂接
 * ================================================================ */
#include "bsp_uart.h"
#include "usart.h"
#include "stm32f4xx_hal.h"

typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t  rx_started;
    uint8_t  tx_busy;
    uint8_t  rx_deferred;
    uint8_t *rx_buf;
    uint16_t rx_len;
    bsp_uart_rx_cb_t rx_cb;
    bsp_uart_tx_cb_t tx_cb;
    void *rx_ctx;
    void *tx_ctx;
} bsp_uart_chan_t;

static bsp_uart_chan_t g_uart[BSP_UART_COUNT] = {
    [BSP_UART_DBG]  = { .huart = &huart3 },   /* 调试口：USART3 (PC10/PC11) */
    [BSP_UART_HOST] = { .huart = &huart1 },
};

static bsp_uart_id_t bsp_uart_id_of(UART_HandleTypeDef *huart)
{
    for (int i = 0; i < BSP_UART_COUNT; i++) {
        if (g_uart[i].huart == huart) return (bsp_uart_id_t)i;
    }
    return BSP_UART_COUNT;
}

void BSP_UART_Init(bsp_uart_id_t id)
{
    if (id >= BSP_UART_COUNT || g_uart[id].huart == NULL) return;
    g_uart[id].rx_started = 0;
    g_uart[id].tx_busy = 0;
    g_uart[id].rx_deferred = 0;
    g_uart[id].rx_buf = NULL;
    g_uart[id].rx_len = 0;
}

void BSP_UART_RegisterRxCb(bsp_uart_id_t id, bsp_uart_rx_cb_t cb, void *ctx)
{
    if (id >= BSP_UART_COUNT) return;
    g_uart[id].rx_cb = cb;
    g_uart[id].rx_ctx = ctx;
}

void BSP_UART_RegisterTxCb(bsp_uart_id_t id, bsp_uart_tx_cb_t cb, void *ctx)
{
    if (id >= BSP_UART_COUNT) return;
    g_uart[id].tx_cb = cb;
    g_uart[id].tx_ctx = ctx;
}

void BSP_UART_RxStart(bsp_uart_id_t id, uint8_t *buf, uint16_t len)
{
    if (id >= BSP_UART_COUNT || buf == NULL || len == 0) return;
    g_uart[id].rx_buf = buf;
    g_uart[id].rx_len = len;
    __HAL_UART_ENABLE_IT(g_uart[id].huart, UART_IT_IDLE);
    HAL_UART_Receive_DMA(g_uart[id].huart, buf, len);
    g_uart[id].rx_started = 1;
}

void BSP_UART_RxStop(bsp_uart_id_t id)
{
    if (id >= BSP_UART_COUNT) return;
    if (g_uart[id].rx_started) {
        HAL_UART_DMAStop(g_uart[id].huart);
        g_uart[id].rx_started = 0;
    }
}

int BSP_UART_TransmitDMA(bsp_uart_id_t id, const uint8_t *data, uint16_t len)
{
    if (id >= BSP_UART_COUNT || data == NULL || len == 0) return -1;
    if (g_uart[id].tx_busy) return -1;

    g_uart[id].tx_busy = 1;
    if (HAL_UART_Transmit_DMA(g_uart[id].huart, (uint8_t *)data, len) != HAL_OK) {
        g_uart[id].tx_busy = 0;
        return -1;
    }
    return 0;
}

/* 中止当前 TX（同步），并复位忙标志。用于 TX 状态自愈。 */
int BSP_UART_AbortTransmit(bsp_uart_id_t id)
{
    if (id >= BSP_UART_COUNT) return -1;
    g_uart[id].tx_busy = 0;
    if (HAL_UART_AbortTransmit(g_uart[id].huart) != HAL_OK) return -1;
    return 0;
}

static void bsp_uart_process_rx(bsp_uart_id_t id)
{
    bsp_uart_chan_t *ch = &g_uart[id];
    uint16_t count;

    HAL_UART_DMAStop(ch->huart);
    count = (uint16_t)(ch->rx_len - __HAL_DMA_GET_COUNTER(ch->huart->hdmarx));
    if (ch->rx_cb && count > 0) {
        ch->rx_cb(id, ch->rx_buf, count, ch->rx_ctx);
    }
    if (ch->rx_started) {
        HAL_UART_Receive_DMA(ch->huart, ch->rx_buf, ch->rx_len);
    }
}

void BSP_UART_IdleISR(bsp_uart_id_t id)
{
    bsp_uart_chan_t *ch;

    if (id >= BSP_UART_COUNT) return;
    ch = &g_uart[id];
    if (!__HAL_UART_GET_FLAG(ch->huart, UART_FLAG_IDLE)) return;
    __HAL_UART_CLEAR_IDLEFLAG(ch->huart);

    /* TX 进行中：推迟 RX 处理到 TX 完成回调，避免 HAL_UART_DMAStop
     * 误中止进行中的 TX DMA（中止后无完成回调，TXTask 将永久挂起）。 */
    if (ch->tx_busy) {
        ch->rx_deferred = 1;
        return;
    }
    bsp_uart_process_rx(id);
}

void BSP_UART_IRQHandler(bsp_uart_id_t id)
{
    if (id >= BSP_UART_COUNT) return;
    HAL_UART_IRQHandler(g_uart[id].huart);
    BSP_UART_IdleISR(id);
}

/* 默认空实现：上层模块（如信号发生器）可重写以扩展非 BSP 通道 */
__weak void BSP_UART_OnTxComplete(void)
{
}

/* 全局 TX 完成回调：路由到对应通道注册的回调 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_id_t id = bsp_uart_id_of(huart);
    if (id == BSP_UART_COUNT) {
        BSP_UART_OnTxComplete();
        return;
    }
    g_uart[id].tx_busy = 0;
    /* 先处理 TX 期间被推迟的 RX 空闲事件（此时 TX 已结束，DMAStop 安全） */
    if (g_uart[id].rx_deferred) {
        g_uart[id].rx_deferred = 0;
        bsp_uart_process_rx(id);
    }
    if (g_uart[id].tx_cb) {
        g_uart[id].tx_cb(id, g_uart[id].tx_ctx);
    }
}

/* 全局 UART 错误回调：确保 TX 忙标志释放并唤醒等待任务，防止永久挂起 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_id_t id = bsp_uart_id_of(huart);
    if (id == BSP_UART_COUNT) return;
    /* 仅当 TX 传输因错误被终止（HAL 已将 gState 复位为 READY）时才唤醒发送任务；
     * RX 错误（过载/帧错误）不影响 TX，不得误清忙标志导致并发 DMA。 */
    if (g_uart[id].tx_busy && huart->gState != HAL_UART_STATE_BUSY_TX) {
        g_uart[id].tx_busy = 0;
        if (g_uart[id].tx_cb) {
            g_uart[id].tx_cb(id, g_uart[id].tx_ctx);
        }
    }
}
