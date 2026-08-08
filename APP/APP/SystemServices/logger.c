#include "logger.h"
#include "bsp.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "stream_buffer.h"

#include <stdarg.h>
#include <stdio.h>

extern StreamBufferHandle_t global_tx_stream;
extern StreamBufferHandle_t global_rx_stream;

/* DMA 缓冲区必须全局且对齐 */
static uint8_t rx_dma_buf[LOG_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_dma_buf[LOG_TX_DMA_CHUNK] __attribute__((aligned(4)));

/* LoggerTX 任务句柄，用于中断通知 */
static TaskHandle_t logger_tx_handle = NULL;

/* ISR：TX DMA 完成，唤醒发送任务 */
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

/* ISR：RX 空闲断帧，数据入流（DMA 由 BSP 自动重启） */
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

void LOG_Init(void)
{
    BSP_UART_Init(BSP_UART_DBG);
    BSP_UART_RegisterRxCb(BSP_UART_DBG, logger_rx_isr, NULL);
    BSP_UART_RegisterTxCb(BSP_UART_DBG, logger_tx_isr, NULL);
    BSP_UART_RxStart(BSP_UART_DBG, rx_dma_buf, sizeof(rx_dma_buf));
}

void LOG_Printf(const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    /* vsnprintf 返回“应写长度”，超长时必须钳制到缓冲内，否则栈越界读 */
    if (len > (int)sizeof(buf) - 1) {
        len = (int)sizeof(buf) - 1;
    }
    if (len > 0) {
        xStreamBufferSend(global_tx_stream, buf, len, 0);
    }
}

void LoggerTXTaskFunction(void)
{
    size_t len;
    logger_tx_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        len = xStreamBufferReceive(global_tx_stream, tx_dma_buf, sizeof(tx_dma_buf), portMAX_DELAY);
        if (len > 0) {
            if (BSP_UART_TransmitDMA(BSP_UART_DBG, tx_dma_buf, len) == 0) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
        }
    }
}

StreamBufferHandle_t LOG_GetRxStream(void)
{
    return global_rx_stream;
}
