#include "data_link.h"
#include "var_manager.h"
#include "pinout.h"
#include "app_config.h"
#include "crc16.h"
#include "cmsis_os.h"
#include "stream_buffer.h"
#include <string.h>
#include "queue.h"
#include "logger.h"
/* ---------- 全局 DMA 缓冲区 ---------- */
static uint8_t rx_dma_buf[HOSTLINK_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_dma_chunk[HOSTLINK_TX_DMA_CHUNK] __attribute__((aligned(4)));

/* ---------- 发送通路 ---------- */
static StreamBufferHandle_t tx_stream;
static TaskHandle_t tx_task_handle = NULL;
static TaskHandle_t cmd_handle = NULL;
/* ---------- 命令队列 ---------- */
typedef struct {
    uint16_t size;
    uint8_t  data[HOSTLINK_RX_DMA_BUF_SIZE];
} cmd_packet_t;
static QueueHandle_t cmd_queue;

/* ---------- 内部函数声明 ---------- */
static void TXTask(void *arg);
static void CmdTask(void *arg);
static void handle_command(const uint8_t *data, uint16_t len);

/* ================== 初始化 ================== */
void DataLink_Init(void)
{
    tx_stream = xStreamBufferCreate(HOSTLINK_TX_STREAM_SIZE, 1);
    configASSERT(tx_stream);
    cmd_queue = xQueueCreate(4, sizeof(cmd_packet_t));
    configASSERT(cmd_queue);

    if (xTaskCreate(TXTask, "DL_TX", 256, NULL, osPriorityHigh, &tx_task_handle) != pdPASS) {
        // 任务创建失败：LED1 快闪
        while (1) {
            HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
            HAL_Delay(100);
        }
    }

    if (xTaskCreate(CmdTask, "DL_CMD", 512, NULL, osPriorityHigh - 1, &cmd_handle) != pdPASS) {
        // 任务创建失败：LED1 快闪
        while (1) {
            HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
            HAL_Delay(100);
        }
    }

    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
    __HAL_UART_ENABLE_IT(&HOSTLINK_UART, UART_IT_IDLE);
}

/* ================== 发送任务 ================== */
static void TXTask(void *arg)
{
    size_t len;
    for (;;) {
        len = xStreamBufferReceive(tx_stream, tx_dma_chunk, sizeof(tx_dma_chunk), portMAX_DELAY);
        if (len > 0) {
            HAL_UART_Transmit_DMA(&HOSTLINK_UART, tx_dma_chunk, len);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 等待 DMA 完成
        }
    }
}

/* ================== 命令处理任务 ================== */
static void CmdTask(void *arg)
{
    cmd_packet_t pkt;
    for (;;) {
        if (xQueueReceive(cmd_queue, &pkt, portMAX_DELAY) == pdTRUE) {

            handle_command(pkt.data, pkt.size);
        }
    }
}

/* ================== ISR 接收回调 (上半部) ================== */
void DataLink_RxIdleCallback(uint16_t size)
{
    HAL_UART_DMAStop(&HOSTLINK_UART);
    if (size >= 7) {
        uint16_t crc_received = rx_dma_buf[size-2] | (rx_dma_buf[size-1] << 8);
        uint16_t crc_calc = CRC16_Calculate(rx_dma_buf, size - 2);
        if (crc_received == crc_calc) {

            cmd_packet_t pkt;
            pkt.size = size - 2 ; // 去掉 CRC
            memcpy(pkt.data, rx_dma_buf, pkt.size);
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(cmd_queue, &pkt, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
}

/* ================== DMA 发送完成回调 (ISR) ================== */
void DataLink_TxCpltCallback(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (tx_task_handle) {
        vTaskNotifyGiveFromISR(tx_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* ================== 发送接口 (任务上下文) ================== */
void DataLink_SendPacket(const uint8_t *data, uint16_t len)
{
    if (len + 2 > HOSTLINK_TX_DMA_CHUNK) return;   // 简单保护
    uint8_t pkt[HOSTLINK_TX_DMA_CHUNK + 2];
    memcpy(pkt, data, len);
    uint16_t crc = CRC16_Calculate(pkt, len);
    pkt[len] = crc & 0xFF;
    pkt[len + 1] = (crc >> 8) & 0xFF;
    xStreamBufferSend(tx_stream, pkt, len + 2, 0);
}

/* ================== 命令处理 ================== */
#define SYNC1 0xAA
#define SYNC2 0x55

static void handle_command(const uint8_t *data, uint16_t len)
{
    uint8_t cmd = data[2];
    uint16_t payload_len = data[3] | (data[4] << 8);   // 小端

    switch (cmd) {
        case 0x01:   // LIST_VARS
            VAR_SendList();
            break;

        case 0x02:   // SUBSCRIBE
            if (payload_len >= 2) {
                VAR_ClearSubscriptions();
                // payload 从 data[5] 开始，每 2 字节一个 ID
                for (uint16_t i = 0; i + 1 < payload_len; i += 2) {
                    uint16_t id = data[5 + i] | (data[5 + i + 1] << 8);
                    VAR_Subscribe(id);
                }
            }
            break;

        case 0x04:   // READ_VAR
            if (payload_len >= 2) {
                uint16_t id = data[5] | (data[6] << 8);
                uint16_t var_len = 0;
                uint8_t val_buf[8] = {0};
                if (VAR_Read(id, val_buf, &var_len) == 0) {
                    // 构建响应帧
                    uint8_t resp[64];
                    resp[0] = 0xAA; resp[1] = 0x55;
                    resp[2] = 0x04;                            // 命令
                    uint16_t resp_payload_len = 4 + var_len;   // ID(2) + len(1) + reserved(1) + value(var_len)
                    resp[3] = resp_payload_len & 0xFF;
                    resp[4] = (resp_payload_len >> 8) & 0xFF;
                    resp[5] = id & 0xFF;
                    resp[6] = (id >> 8) & 0xFF;
                    resp[7] = var_len & 0xFF;                  // 值长度
                    resp[8] = 0;                               // 保留
                    memcpy(&resp[9], val_buf, var_len);
                    DataLink_SendPacket(resp, 9 + var_len);
                }
            }
            break;

        case 0x05:   // WRITE_VAR
            if (payload_len >= 4) {
                uint16_t id = data[5] | (data[6] << 8);
                uint8_t wlen = data[7];                       // 值长度
                // 实际数据从 data[8] 开始
                VAR_Write(id, &data[8], wlen);
            }
            break;

        default:
            break;
    }
}