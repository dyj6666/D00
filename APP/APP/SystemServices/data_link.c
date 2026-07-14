#include "data_link.h"
#include "var_manager.h"
#include "pinout.h"
#include "app_config.h"
#include "crc16.h"
#include "cmsis_os.h"
#include "stream_buffer.h"
#include "timers.h"
#include "usart.h"
#include <string.h>
#include "protocol.h"

static StreamBufferHandle_t tx_stream;
static uint8_t rx_dma_buf[HOSTLINK_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_dma_chunk[HOSTLINK_TX_DMA_CHUNK] __attribute__((aligned(4)));
static TaskHandle_t tx_task_handle = NULL;

static void handle_command(const uint8_t *pkt, uint16_t len);

/* 协议常量 */
#define SYNC1  0xAA
#define SYNC2  0x55
#define CMD_LIST_VARS   0x01
#define CMD_SUBSCRIBE   0x02
#define CMD_DATA        0x03
#define CMD_READ_VAR    0x04
#define CMD_WRITE_VAR   0x05

static void TXTask(void *arg) {
    tx_task_handle = xTaskGetCurrentTaskHandle();
    size_t len;
    for (;;) {
        len = xStreamBufferReceive(tx_stream, tx_dma_chunk, sizeof(tx_dma_chunk), portMAX_DELAY);
        if (len > 0) {
            HAL_UART_Transmit_DMA(&HOSTLINK_UART, tx_dma_chunk, len);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待 DMA 完成
        }
    }
}

void DataLink_SendPacket(const uint8_t *data, uint16_t len) {
    if (len > HOSTLINK_TX_DMA_CHUNK) return; // 简单截断，实际可分片
    // 添加 CRC16 并发送
    uint8_t pkt[len + 2];
    memcpy(pkt, data, len);
    uint16_t crc = CRC16_Calculate(pkt, len);
    pkt[len] = crc & 0xFF;
    pkt[len+1] = (crc >> 8) & 0xFF;
    xStreamBufferSend(tx_stream, pkt, len + 2, 0);
}

void DataLink_Init(void) {
    tx_stream = xStreamBufferCreate(HOSTLINK_TX_STREAM_SIZE, 1);
    configASSERT(tx_stream);
    xTaskCreate(TXTask, "DL_TX", 256, NULL, osPriorityHigh, &tx_task_handle);
    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
    __HAL_UART_ENABLE_IT(&HOSTLINK_UART, UART_IT_IDLE);
}

// DMA 发送完成回调（在 stm32f4xx_it.c 的 USART3_IRQHandler 中调用）
void DataLink_TxCpltCallback(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (tx_task_handle) {
        vTaskNotifyGiveFromISR(tx_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
static void handle_command(const uint8_t *pkt, uint16_t len) {
    uint8_t cmd = pkt[2];
    uint16_t payload_len = pkt[3] | (pkt[4] << 8);

    switch (cmd) {
        case CMD_LIST_VARS:
            VAR_SendList();
            break;
        case CMD_SUBSCRIBE:
            VAR_ClearSubscriptions();
            for (uint16_t i = 5; i < 5 + payload_len; i += 2) {
                uint16_t id = pkt[i] | (pkt[i+1] << 8);
                VAR_Subscribe(id);
            }
            break;
        case CMD_READ_VAR:
            if (payload_len >= 2) {
                uint16_t id = pkt[5] | (pkt[6] << 8);
                uint16_t var_len;
                uint8_t val_buf[8];
                if (VAR_Read(id, val_buf, &var_len) == 0) {
                    uint8_t resp[64];
                    resp[0] = SYNC1; resp[1] = SYNC2;
                    resp[2] = CMD_READ_VAR;
                    resp[3] = 4 + var_len;
                    resp[4] = (4 + var_len) >> 8;
                    resp[5] = id & 0xFF;
                    resp[6] = (id >> 8) & 0xFF;
                    resp[7] = var_len & 0xFF;
                    resp[8] = 0;
                    memcpy(&resp[9], val_buf, var_len);
                    DataLink_SendPacket(resp, 9 + var_len);
                }
            }
            break;
        case CMD_WRITE_VAR:
            if (payload_len >= 4) {
                uint16_t id = pkt[5] | (pkt[6] << 8);
                uint16_t wlen = pkt[7];
                VAR_Write(id, &pkt[8], wlen);
            }
            break;
    }
}
// 空闲中断回调
void DataLink_RxIdleCallback(uint16_t size) {
    if (size < 6) return;
    // CRC16 校验（最后2字节为CRC）
    uint16_t crc_received = rx_dma_buf[size-2] | (rx_dma_buf[size-1] << 8);
    uint16_t crc_calc = CRC16_Calculate(rx_dma_buf, size - 2);
    if (crc_received != crc_calc) return;

    handle_command(rx_dma_buf, size - 2); // 传入不带CRC的纯数据

    HAL_UART_DMAStop(&HOSTLINK_UART);
    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
}