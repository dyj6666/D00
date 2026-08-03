#include "data_link.h"
#include "bsp.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "crc16.h"
#include "FreeRTOS.h"
#include "protocol.h"
#include "queue.h"
#include "stream_buffer.h"
#include "var_manager.h"

#include <string.h>

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
static volatile uint32_t g_cmd_lost = 0;    /* 命令队列溢出计数 */

/* ---------- 内部函数声明 ---------- */
static void TXTask(void *arg);
static void CmdTask(void *arg);
static void handle_command(const uint8_t *data, uint16_t len);
static void send_error_response(uint8_t orig_cmd, uint8_t err_code);
static void data_link_rx_isr(bsp_uart_id_t id, const uint8_t *data,
                             uint16_t len, void *ctx);
static void data_link_tx_isr(bsp_uart_id_t id, void *ctx);

/* ================== 初始化 ================== */
void DataLink_Init(void)
{
    osThreadAttr_t tx_attr = {
        .name = "DL_TX",
        .stack_size = 1024,
        .priority = osPriorityLow,
    };
    osThreadAttr_t cmd_attr = {
        .name = "DL_CMD",
        .stack_size = 2048,
        .priority = osPriorityLow,
    };

    tx_stream = xStreamBufferCreate(HOSTLINK_TX_STREAM_SIZE, 1);
    configASSERT(tx_stream);
    cmd_queue = xQueueCreate(HOSTLINK_CMD_QUEUE_LEN, sizeof(cmd_packet_t));
    configASSERT(cmd_queue);

    tx_task_handle = (TaskHandle_t)osThreadNew(TXTask, NULL, &tx_attr);
    cmd_handle = (TaskHandle_t)osThreadNew(CmdTask, NULL, &cmd_attr);
    if (tx_task_handle == NULL || cmd_handle == NULL) {
        /* 任务创建失败：LED1 快闪 */
        while (1) {
            BSP_LED_Toggle(1);
            BSP_DelayMs(100);
        }
    }

    BSP_UART_Init(BSP_UART_HOST);
    BSP_UART_RegisterRxCb(BSP_UART_HOST, data_link_rx_isr, NULL);
    BSP_UART_RegisterTxCb(BSP_UART_HOST, data_link_tx_isr, NULL);
    BSP_UART_RxStart(BSP_UART_HOST, rx_dma_buf, sizeof(rx_dma_buf));
}

/* ================== 发送任务 ================== */
static void TXTask(void *arg)
{
    size_t len;
    (void)arg;
    for (;;) {
        len = xStreamBufferReceive(tx_stream, tx_dma_chunk, sizeof(tx_dma_chunk), portMAX_DELAY);
        if (len > 0) {
            if (BSP_UART_TransmitDMA(BSP_UART_HOST, tx_dma_chunk, len) == 0) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* 等待 DMA 完成 */
            }
        }
    }
}

/* ================== 命令处理任务 ================== */
static void CmdTask(void *arg)
{
    cmd_packet_t pkt;
    (void)arg;
    for (;;) {
        if (xQueueReceive(cmd_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            handle_command(pkt.data, pkt.size);
        }
    }
}

/* ================== ISR 回调（上半部） ================== */
static void data_link_tx_isr(bsp_uart_id_t id, void *ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)id;
    (void)ctx;
    if (tx_task_handle) {
        vTaskNotifyGiveFromISR(tx_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void data_link_rx_isr(bsp_uart_id_t id, const uint8_t *data,
                             uint16_t len, void *ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    const uint8_t *frame = NULL;
    uint16_t frame_len = 0;
    (void)id;
    (void)ctx;

    /* 先整帧校验（含 CRC）；失败时滑动重同步，尽量从残留字节恢复合法帧 */
    if (len >= PROTOCOL_HEADER_LEN + PROTOCOL_CRC_LEN &&
        Protocol_ValidateFrame(data, len) == PROTO_ERR_NONE) {
        frame = data;
        frame_len = len;
    } else {
        for (uint16_t off = 1; off + PROTOCOL_HEADER_LEN + PROTOCOL_CRC_LEN <= len; off++) {
            if (data[off - 1] == SYNC1 && data[off] == SYNC2) {
                uint16_t remain = len - (off - 1);
                if (Protocol_ValidateFrame(&data[off - 1], remain) == PROTO_ERR_NONE) {
                    frame = &data[off - 1];
                    frame_len = remain;
                    break;
                }
            }
        }
    }

    if (frame != NULL) {
        cmd_packet_t pkt;
        pkt.size = frame_len - PROTOCOL_CRC_LEN;
        memcpy(pkt.data, frame, pkt.size);
        if (xQueueSendFromISR(cmd_queue, &pkt, &xHigherPriorityTaskWoken) != pdTRUE) {
            g_cmd_lost++;
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

uint32_t DataLink_GetCmdLostCount(void)
{
    return g_cmd_lost;
}

/* ================== 发送接口（任务上下文） ================== */
/* data/len: 不含 CRC 的帧数据（帧头+payload），函数负责追加 CRC 并发送。 */
int DataLink_SendPacket(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < PROTOCOL_HEADER_LEN) return -1;
    if ((uint32_t)len + PROTOCOL_CRC_LEN > HOSTLINK_TX_DMA_CHUNK) return -1;

    /* 局部缓冲，避免与 TXTask 对 tx_dma_chunk 的并发读写竞争 */
    uint8_t pkt[HOSTLINK_TX_DMA_CHUNK + PROTOCOL_CRC_LEN];
    memcpy(pkt, data, len);
    uint16_t crc = CRC16_Calculate(pkt, len);
    pkt[len] = (uint8_t)(crc & 0xFF);
    pkt[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    xStreamBufferSend(tx_stream, pkt, len + PROTOCOL_CRC_LEN, 0);
    return 0;
}

/* 组装完整帧（含 CRC）并发送。 */
int DataLink_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[HOSTLINK_TX_DMA_CHUNK];
    uint16_t frame_len = 0;
    int err = Protocol_BuildFrame(frame, sizeof(frame), cmd, payload, payload_len, &frame_len);
    if (err != PROTO_ERR_NONE) return -1;
    return DataLink_SendPacket(frame, frame_len - PROTOCOL_CRC_LEN);
}

/* ================== 命令处理 ================== */
static void send_error_response(uint8_t orig_cmd, uint8_t err_code)
{
    uint8_t payload[PROTOCOL_ERR_PAYLOAD_LEN];
    payload[0] = orig_cmd;
    payload[1] = err_code;
    payload[2] = 0;
    payload[3] = 0;
    DataLink_SendFrame(CMD_ERROR, payload, sizeof(payload));
}

static void handle_command(const uint8_t *data, uint16_t len)
{
    protocol_frame_t f;
    int err = Protocol_ParseHeader(data, len, &f);
    if (err != PROTO_ERR_NONE) {
        uint8_t cmd = (len >= 3) ? data[2] : 0xFF;
        send_error_response(cmd, (uint8_t)err);
        return;
    }

    switch (f.cmd) {
        case CMD_LIST_VARS:
            if (f.payload_len != 0) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            VAR_SendList();
            break;

        case CMD_SUBSCRIBE: {
            if (f.payload_len < 2 || (f.payload_len % 2) != 0) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            VAR_ClearSubscriptions();
            for (uint16_t i = 0; i + 1 < f.payload_len; i += 2) {
                uint16_t id = (uint16_t)(f.payload[i] | (f.payload[i + 1] << 8));
                VAR_Subscribe(id);
            }
            break;
        }

        case CMD_READ_VAR: {
            if (f.payload_len < 2) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint16_t id = (uint16_t)(f.payload[0] | (f.payload[1] << 8));
            uint16_t var_len = 0;
            uint8_t val_buf[8] = {0};
            if (VAR_Read(id, val_buf, &var_len) != 0) {
                send_error_response(f.cmd, PROTO_ERR_VAR_NOT_FOUND);
                break;
            }
            /* 响应 payload: ID(2) + len(1) + reserved(1) + value */
            uint8_t payload[2 + 1 + 1 + 8];
            payload[0] = id & 0xFF;
            payload[1] = (id >> 8) & 0xFF;
            payload[2] = (uint8_t)var_len;
            payload[3] = 0;
            memcpy(&payload[4], val_buf, var_len);
            DataLink_SendFrame(CMD_READ_VAR, payload, 4 + var_len);
            break;
        }

        case CMD_WRITE_VAR: {
            if (f.payload_len < 4) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint16_t id = (uint16_t)(f.payload[0] | (f.payload[1] << 8));
            uint8_t wlen = f.payload[2];
            /* 布局兼容原协议: id(2) + wlen(1) + value(wlen) */
            if ((uint16_t)wlen > f.payload_len - 3) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            if (VAR_Write(id, &f.payload[3], wlen) != 0) {
                send_error_response(f.cmd, PROTO_ERR_VAR_NOT_FOUND);
            }
            break;
        }

        case CMD_GET_INFO: {
            uint8_t ver[4];
            Protocol_GetVersion(ver);
            DataLink_SendFrame(CMD_GET_INFO, ver, sizeof(ver));
            break;
        }

        default:
            send_error_response(f.cmd, PROTO_ERR_UNKNOWN_CMD);
            break;
    }
}
