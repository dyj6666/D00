#include "data_link.h"
#include "var_manager.h"
#include "pinout.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "stream_buffer.h"  // 不需要，直接用全局缓冲
#include <string.h>
#include "task.h"

/* DMA 缓冲区 */
static uint8_t rx_dma_buf[HOSTLINK_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_dma_buf[HOSTLINK_TX_DMA_BUF_SIZE] __attribute__((aligned(4)));

static TaskHandle_t tx_task_handle = NULL;

/* 协议命令字 */
#define CMD_LIST_VARS       0x01
#define CMD_SUBSCRIBE       0x02
#define CMD_DATA            0x03
#define CMD_READ_VAR        0x04
#define CMD_WRITE_VAR       0x05

/* 帧结构：sync(2B) + cmd(1B) + len(2B) + payload(N) + crc(1B) */
#define SYNC1 0xAA
#define SYNC2 0x55

static void send_variable_list(void) {
    // 发送所有注册变量的信息（id, name, type, permission）
    uint8_t buf[512];
    uint16_t idx = 5; // 跳过 sync+cmd+len
    buf[0] = SYNC1; buf[1] = SYNC2;
    buf[2] = CMD_LIST_VARS;

    for (int i = 0; i < reg_count; i++) {
        // 简单格式：id(2B) + type(1B) + perm(1B) + name_len(1B) + name
        buf[idx++] = registry[i].id & 0xFF;
        buf[idx++] = (registry[i].id >> 8) & 0xFF;
        buf[idx++] = registry[i].type;
        buf[idx++] = registry[i].permission;
        uint8_t name_len = strlen(registry[i].name);
        buf[idx++] = name_len;
        memcpy(&buf[idx], registry[i].name, name_len);
        idx += name_len;
        if (idx > 500) break; // 防止溢出
    }

    uint16_t payload_len = idx - 5;
    buf[3] = payload_len & 0xFF;
    buf[4] = (payload_len >> 8) & 0xFF;
    uint8_t crc = 0;
    for (uint16_t i = 0; i < idx; i++) crc ^= buf[i];
    buf[idx++] = crc;

    DataLink_SendPacket(buf, idx);
}

static void handle_command(const uint8_t *pkt, uint16_t len) {
    uint8_t cmd = pkt[2];
    uint16_t payload_len = pkt[3] | (pkt[4] << 8);

    switch (cmd) {
        case CMD_LIST_VARS:
            send_variable_list();
            break;

        case CMD_SUBSCRIBE: {
            VAR_ClearSubscriptions();
            for (uint16_t i = 5; i < 5 + payload_len; i += 2) {
                uint16_t id = pkt[i] | (pkt[i+1] << 8);
                VAR_Subscribe(id);
            }
            break;
        }

        case CMD_READ_VAR: {
            if (payload_len >= 2) {
                uint16_t id = pkt[5] | (pkt[6] << 8);
                uint16_t var_len;
                uint8_t val_buf[8];
                if (VAR_Read(id, val_buf, &var_len) == 0) {
                    uint8_t resp[64];
                    resp[0] = SYNC1; resp[1] = SYNC2;
                    resp[2] = CMD_READ_VAR;
                    resp[3] = 4 + var_len;  // payload length
                    resp[4] = (4 + var_len) >> 8;
                    resp[5] = id & 0xFF;
                    resp[6] = (id >> 8) & 0xFF;
                    resp[7] = var_len & 0xFF;
                    resp[8] = 0; // 保留
                    memcpy(&resp[9], val_buf, var_len);
                    uint8_t crc = 0;
                    for (int i = 0; i < 9 + var_len; i++) crc ^= resp[i];
                    resp[9 + var_len] = crc;
                    DataLink_SendPacket(resp, 9 + var_len + 1);
                }
            }
            break;
        }

        case CMD_WRITE_VAR: {
            if (payload_len >= 4) {
                uint16_t id = pkt[5] | (pkt[6] << 8);
                uint16_t wlen = pkt[7];
                VAR_Write(id, &pkt[8], wlen);
            }
            break;
        }
    }
}

void DataLink_RxIdleCallback(uint16_t size) {
    if (size < 6) return;
    // 校验帧头
    if (rx_dma_buf[0] != SYNC1 || rx_dma_buf[1] != SYNC2) return;
    uint8_t cmd = rx_dma_buf[2];
    uint16_t len = rx_dma_buf[3] | (rx_dma_buf[4] << 8);
    if (size < 6 + len) return;
    uint8_t crc = 0;
    for (uint16_t i = 0; i < 5 + len; i++) crc ^= rx_dma_buf[i];
    if (crc != rx_dma_buf[5 + len]) return;

    handle_command(rx_dma_buf, size);

    // 重启 DMA
    HAL_UART_DMAStop(&HOSTLINK_UART);
    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
}

static void TXTask(void *arg) {
    tx_task_handle = xTaskGetCurrentTaskHandle();
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // 通知到来时，实际发送已在 SendPacket 中触发，这里仅作同步用
    }
}

void DataLink_SendPacket(const uint8_t *data, uint16_t len) {
    // 保护临界区，防止重入
    static volatile uint8_t tx_busy = 0;
    while (tx_busy) { vTaskDelay(1); }
    tx_busy = 1;
    memcpy(tx_dma_buf, data, len < sizeof(tx_dma_buf) ? len : sizeof(tx_dma_buf));
    HAL_UART_Transmit_DMA(&HOSTLINK_UART, tx_dma_buf, len);
    // 等待 DMA 完成
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    tx_busy = 0;
}

void DataLink_Init(void) {
    // 创建发送任务
    xTaskCreate(TXTask, "DL_TX", 256, NULL, osPriorityHigh, &tx_task_handle);
    // 启动 DMA 接收
    HAL_UART_Receive_DMA(&HOSTLINK_UART, rx_dma_buf, sizeof(rx_dma_buf));
    __HAL_UART_ENABLE_IT(&HOSTLINK_UART, UART_IT_IDLE);
}