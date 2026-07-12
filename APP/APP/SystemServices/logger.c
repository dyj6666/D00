#include "logger.h"
#include "pinout.h"
#include "app_config.h"
#include "main.h"
#include "usart.h"
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

void LOG_Init(void)
{
    // 仅初始化硬件，不再创建 StreamBuffer
    HAL_UART_Receive_DMA(&DEBUG_UART, rx_dma_buf, sizeof(rx_dma_buf));
    __HAL_UART_ENABLE_IT(&DEBUG_UART, UART_IT_IDLE);
}

void LOG_Printf(const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (len > 0) {
        xStreamBufferSend(global_tx_stream, buf, len, 0);
    }
}

void LoggerTXTaskFunction(void)
{
    logger_tx_handle = xTaskGetCurrentTaskHandle();
    size_t len;
    for (;;) {
        len = xStreamBufferReceive(global_tx_stream, tx_dma_buf, sizeof(tx_dma_buf), portMAX_DELAY);
        if (len > 0) {
            HAL_UART_Transmit_DMA(&DEBUG_UART, tx_dma_buf, len);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

/* DMA 发送完成回调（来自中断） */
void LOG_TxCpltCallback(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (logger_tx_handle != NULL) {
        vTaskNotifyGiveFromISR(logger_tx_handle, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* 空闲中断回调（来自中断） */
void LOG_RxIdleCallback(uint16_t size)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (size > 0) {
        xStreamBufferSendFromISR(global_rx_stream, rx_dma_buf, size, &xHigherPriorityTaskWoken);
    }
    /* 重启 DMA 接收 */
    HAL_UART_DMAStop(&DEBUG_UART);
    HAL_UART_Receive_DMA(&DEBUG_UART, rx_dma_buf, sizeof(rx_dma_buf));
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

StreamBufferHandle_t LOG_GetRxStream(void)
{
    return global_rx_stream;
}
