/* ================================================================
 * logger —— 日志系统实现：DMA 收发 + 流缓冲 + 发送任务
 *
 * 架构位置：APP 服务层；LOG_Printf 全模块入口
 * 核心流程：Printf 入 TX 流 -> LoggerTXTask 搬运 DMA -> ISR 完成唤醒
 * 关键约束：DMA 缓冲必须全局对齐；RX 流同时供 Shell 命令输入
 * ================================================================ */
#include "logger.h"
#include "bsp.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "stream_buffer.h"
#include "stm32f4xx.h"

#include <stdarg.h>
#include <stdio.h>

extern StreamBufferHandle_t global_tx_stream;
extern StreamBufferHandle_t global_rx_stream;

/* DMA 缓冲区必须全局且对齐，否则搬运会硬件出错 */
static uint8_t rx_dma_buf[LOG_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_dma_buf[LOG_TX_DMA_CHUNK] __attribute__((aligned(4)));

/* LoggerTX 任务句柄：ISR 中唤醒发送任务 */
static TaskHandle_t logger_tx_handle = NULL;

/* TX DMA 超时自愈计数（通知丢失/中止等，可经 sysmon 查询） */
static volatile uint32_t s_tx_err = 0;

/** @brief TX DMA 完成中断：唤醒发送任务继续搬运下一块 */
static void logger_tx_isr(bsp_uart_id_t id, void *ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)id;
    (void)ctx;
    if (logger_tx_handle != NULL) {
        vTaskNotifyGiveFromISR(logger_tx_handle, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/** @brief RX 空闲断帧中断：命令数据入流（DMA 由 BSP 自动重启） */
static void logger_rx_isr(bsp_uart_id_t id, const uint8_t *data,
                          uint16_t len, void *ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)id;
    (void)ctx;
    if (len > 0) {
        xStreamBufferSendFromISR(global_rx_stream, data, len, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static log_sink_fn s_sink = NULL;

/** @brief 设置输出路由钩子；NULL=恢复默认串口输出 */
void LOG_SetSink(log_sink_fn fn)
{
    s_sink = fn;
}

/** @brief 原始输出：直接入 TX 流（不经路由钩子） */
void LOG_WriteRaw(const char *s, uint16_t len)
{
    if (s != NULL && len > 0) {
        xStreamBufferSend(global_tx_stream, s, len, 0);
    }
}

/** @brief 初始化日志：挂 DMA 收发回调并启动 RX */
void LOG_Init(void)
{
    BSP_UART_Init(BSP_UART_DBG);
    BSP_UART_RegisterRxCb(BSP_UART_DBG, logger_rx_isr, NULL);
    BSP_UART_RegisterTxCb(BSP_UART_DBG, logger_tx_isr, NULL);
    BSP_UART_RxStart(BSP_UART_DBG, rx_dma_buf, sizeof(rx_dma_buf));
}

/**
 * @brief  格式化日志输出
 * @note   命令分发期间经路由钩子导到当前适配器；
 *         中断上下文永远走原始串口（避免 ISR 内写网络）
 */
void LOG_Printf(const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    /* vsnprintf 返回"应写长度"：超长必须钳制到缓冲内，否则栈越界读 */
    if (len > (int)sizeof(buf) - 1) {
        len = (int)sizeof(buf) - 1;
    }
    if (len > 0) {
        /* 命令分发期间由统一命令框架把输出路由到当前适配器；
         * 中断上下文（如崩溃记录）永远走原始串口，避免在 ISR 写网络。 */
        if (s_sink != NULL && __get_IPSR() == 0U) {
            s_sink(buf, (uint16_t)len);
        } else {
            LOG_WriteRaw(buf, (uint16_t)len);
        }
    }
}

/** @brief 日志发送任务：流缓冲 -> DMA 逐块搬运（等待完成再取下一块） */
void LoggerTXTaskFunction(void)
{
    size_t len;
    logger_tx_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        len = xStreamBufferReceive(global_tx_stream, tx_dma_buf, sizeof(tx_dma_buf), portMAX_DELAY);
        if (len > 0) {
            if (BSP_UART_TransmitDMA(BSP_UART_DBG, tx_dma_buf, len) == 0) {
                /* 与 data_link 一致的超时自愈：通知丢失/中止不再永久挂起 */
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
                    BSP_UART_AbortTransmit(BSP_UART_DBG);
                    ulTaskNotifyTake(pdTRUE, 0);   /* 清除竞态残留通知 */
                    s_tx_err++;
                }
            }
        }
    }
}

StreamBufferHandle_t LOG_GetRxStream(void)
{
    return global_rx_stream;
}

uint32_t LOG_GetTxErrCount(void)
{
    return s_tx_err;
}
