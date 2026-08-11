/* ================================================================
 * data_link —— HOSTLINK 串口协议链路（DMA 收发 + 双任务队列）
 *
 * 架构位置：APP 服务层；对上提供 DataLink_Send* 接口，对下挂 BSP UART
 * 核心流程：RX DMA -> ISR 上半部 -> CmdTask 解析分发 -> 响应帧进 TX 队列
 *           -> TXTask 整帧发送；命令含 OTA_BEGIN/DATA/END 转发 OtaAgent
 * 关键约束：整帧队列保证帧边界；命令队列溢出计数 g_cmd_lost 可查
 * ================================================================ */
#include "data_link.h"
#include "bsp.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "crc16.h"
#include "FreeRTOS.h"
#include "protocol.h"
#include "queue.h"
#include "task.h"
#include "var_manager.h"
#include "la_sample.h"
#include "ota_agent.h"

#include <string.h>

/* ---------------- 全局 DMA 缓冲区 ---------------- */
static uint8_t rx_dma_buf[HOSTLINK_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));  /* RX DMA 常驻缓冲 */
static uint8_t tx_dma_chunk[HOSTLINK_TX_DMA_CHUNK] __attribute__((aligned(4)));   /* TX DMA 搬运缓冲 */

/* ---------------- 发送通路 ---------------- */
typedef struct {
    uint16_t len;                    /* 帧总长（含帧头/CRC） */
    uint8_t  data[HOSTLINK_TX_FRAME_MAX];  /* 整帧内容，按值入队 */
} tx_frame_t;
static QueueHandle_t tx_queue;       /* 整帧队列：保证帧边界，防大块截断 */
static TaskHandle_t tx_task_handle = NULL;  /* 发送任务句柄 */
static TaskHandle_t cmd_handle = NULL;      /* 命令处理任务句柄 */

/* ---------------- 命令队列 ---------------- */
typedef struct {
    uint16_t size;                   /* 有效载荷长度 */
    uint8_t  data[HOSTLINK_RX_DMA_BUF_SIZE];  /* 帧数据（DMA 缓冲拷贝） */
} cmd_packet_t;
static QueueHandle_t cmd_queue;      /* 命令帧队列：突发不丢帧 */
static volatile uint32_t g_cmd_lost = 0;    /* 命令队列溢出计数 */
static volatile uint32_t g_tx_lost = 0;     /* TX 流缓冲溢出计数 */
static volatile uint32_t g_tx_err = 0;      /* TX DMA 异常/超时自愈计数 */

/* ---------------- 内部函数声明 ---------------- */
static void TXTask(void *arg);
static void CmdTask(void *arg);
static void handle_command(const uint8_t *data, uint16_t len);
static void send_error_response(uint8_t orig_cmd, uint8_t err_code);
static void data_link_rx_isr(bsp_uart_id_t id, const uint8_t *data,
                             uint16_t len, void *ctx);
static void data_link_tx_isr(bsp_uart_id_t id, void *ctx);

/* ---------------- 初始化 ---------------- */
/** @brief 初始化 HOSTLINK：建队列、起 TX/Cmd 双任务、挂 DMA 中断回调 */
void DataLink_Init(void)
{
    osThreadAttr_t tx_attr = {
        .name = "DL_TX",
        .stack_size = 1024,          /* 纯搬运任务，栈无需过大 */
        .priority = osPriorityLow,
    };
    osThreadAttr_t cmd_attr = {
        .name = "DL_CMD",
        .stack_size = 2048,          /* 命令解析含协议栈调用 */
        .priority = osPriorityLow,
    };

    tx_queue = xQueueCreate(HOSTLINK_TX_QUEUE_LEN, sizeof(tx_frame_t));  /* 整帧队列 */
    configASSERT(tx_queue);
    cmd_queue = xQueueCreate(HOSTLINK_CMD_QUEUE_LEN, sizeof(cmd_packet_t));  /* 命令队列 */
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
    tx_frame_t frame;
    (void)arg;
    for (;;) {
        if (xQueueReceive(tx_queue, &frame, portMAX_DELAY) == pdTRUE) {
            memcpy(tx_dma_chunk, frame.data, frame.len);
            /* 等待 DMA 完成（带超时自愈：任何丢通知/中止/错误都不再永久挂起） */
            if (BSP_UART_TransmitDMA(BSP_UART_HOST, tx_dma_chunk, frame.len) != 0) {
                BSP_UART_AbortTransmit(BSP_UART_HOST);
                g_tx_err++;
                continue;
            }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
                BSP_UART_AbortTransmit(BSP_UART_HOST);
                ulTaskNotifyTake(pdTRUE, 0);    /* 清除竞态下残留的通知 */
                g_tx_err++;
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

/* ---------------- 发送接口（任务上下文） ---------------- */
/**
 * @brief  发送一帧数据（帧头+payload，自动追加 CRC 后入 TX 队列）
 * @param  data  不含 CRC 的帧数据（帧头+payload）
 * @param  len   数据长度，不得小于帧头长度
 * @return 0=入队成功；-1=参数非法或超长
 * @note   队列满时丢帧并计数 g_tx_lost，调用方按需使用 Wait 版本
 */
int DataLink_SendPacket(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < PROTOCOL_HEADER_LEN) return -1;
    if ((uint32_t)len + PROTOCOL_CRC_LEN > HOSTLINK_TX_FRAME_MAX) return -1;

    tx_frame_t frame;
    memcpy(frame.data, data, len);               /* 帧内容按值拷贝入队 */
    uint16_t crc = CRC16_Calculate(frame.data, len);  /* MODBUS CRC-16 */
    frame.data[len] = (uint8_t)(crc & 0xFF);     /* CRC 低字节 */
    frame.data[len + 1] = (uint8_t)((crc >> 8) & 0xFF);  /* CRC 高字节 */
    frame.len = len + PROTOCOL_CRC_LEN;          /* 帧总长 = 数据 + 2B CRC */
    if (xQueueSend(tx_queue, &frame, 0) != pdTRUE) {
        g_tx_lost++;
    }
    return 0;
}

uint32_t DataLink_GetTxLostCount(void)
{
    return g_tx_lost;
}

uint32_t DataLink_GetTxErrorCount(void)
{
    return g_tx_err;
}

/** @brief 按命令码组装完整帧（含 CRC）并入发送队列 */
int DataLink_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[HOSTLINK_TX_DMA_CHUNK];
    uint16_t frame_len = 0;
    int err = Protocol_BuildFrame(frame, sizeof(frame), cmd, payload, payload_len, &frame_len);
    if (err != PROTO_ERR_NONE) return -1;
    return DataLink_SendPacket(frame, frame_len - PROTOCOL_CRC_LEN);
}

/**
 * @brief  背压方式发送完整帧：队列满时阻塞等待，最多 timeout_ms
 * @param  cmd         命令码
 * @param  payload     载荷指针
 * @param  payload_len 载荷长度
 * @param  timeout_ms  最长等待时间
 * @return 0=成功；-1=构建失败/超长/超时
 * @note   用于大块可靠导出（如 LA_DUMP），与发送速率自匹配，绝不静默丢帧
 */
int DataLink_SendFrameWait(uint8_t cmd, const uint8_t *payload,
                           uint16_t payload_len, uint32_t timeout_ms)
{
    uint8_t frame[HOSTLINK_TX_DMA_CHUNK];
    uint16_t frame_len = 0;
    tx_frame_t tf;

    if (Protocol_BuildFrame(frame, sizeof(frame), cmd, payload,
                            payload_len, &frame_len) != PROTO_ERR_NONE) {
        return -1;
    }
    if (frame_len > HOSTLINK_TX_FRAME_MAX) return -1;

    memcpy(tf.data, frame, frame_len);
    tf.len = frame_len;
    if (xQueueSend(tx_queue, &tf, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }
    return 0;
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

        case CMD_LA_DUMP: {
            /* 请求：offset(u32 LE) + count(u32 LE)
             * 响应：多帧 CMD_LA_DUMP，payload = offset(u32) + sent(u16) + samples(4B each)
             * 从逻辑分析仪 DMA 缓冲导出原始采样（二进制，供上位机分析）。 */
            if (f.payload_len != 8) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint32_t offset = (uint32_t)(f.payload[0] | (f.payload[1] << 8) |
                                         (f.payload[2] << 16) | (f.payload[3] << 24));
            uint32_t count = (uint32_t)(f.payload[4] | (f.payload[5] << 8) |
                                        (f.payload[6] << 16) | (f.payload[7] << 24));

            uint32_t buf_size = LA_Sample_GetDMABufferSize();
            if (offset >= buf_size || count == 0) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            if (count > buf_size - offset) count = buf_size - offset;

            /* 每帧最多 60 个样本；背压式发送（队列满则阻塞），与 921600
             * 波特率排空速度自匹配，不再固定延时、绝不静默丢帧 */
            uint8_t samples[60 * 4];
            uint32_t sent = 0;
            while (sent < count) {
                uint32_t n = count - sent;
                if (n > 60) n = 60;
                LA_Sample_ReadDMABuffer((uint32_t *)samples, offset + sent, n);

                uint8_t payload[4 + 2 + 60 * 4];
                payload[0] = offset & 0xFF;
                payload[1] = (offset >> 8) & 0xFF;
                payload[2] = (offset >> 16) & 0xFF;
                payload[3] = (offset >> 24) & 0xFF;
                payload[4] = (uint8_t)(n & 0xFF);
                payload[5] = (uint8_t)((n >> 8) & 0xFF);
                memcpy(&payload[6], samples, n * 4);
                if (DataLink_SendFrameWait(CMD_LA_DUMP, payload,
                                           6 + n * 4, 500) != 0) {
                    /* TX 通道异常（队列满 500ms 仍无法排空）：中止导出 */
                    send_error_response(f.cmd, PROTO_ERR_BUF_TOO_SMALL);
                    break;
                }
                sent += n;
            }
            break;
        }

        case CMD_OTA_BEGIN: {
            /* 请求：version(u32 LE) + size(u32 LE) */
            if (f.payload_len != 8) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint32_t ver = (uint32_t)(f.payload[0] | (f.payload[1] << 8) |
                                      (f.payload[2] << 16) | (f.payload[3] << 24));
            uint32_t size = (uint32_t)(f.payload[4] | (f.payload[5] << 8) |
                                       (f.payload[6] << 16) | (f.payload[7] << 24));
            uint8_t st = Ota_Begin(ver, size);
            DataLink_SendFrame(CMD_OTA_BEGIN, &st, 1);
            break;
        }

        case CMD_OTA_DATA: {
            /* 请求：offset(u32 LE) + data(<=120B) */
            if (f.payload_len < 5 || f.payload_len > 4 + OTA_CHUNK_MAX) {
                send_error_response(f.cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint32_t off = (uint32_t)(f.payload[0] | (f.payload[1] << 8) |
                                      (f.payload[2] << 16) | (f.payload[3] << 24));
            uint8_t st = Ota_Data(off, &f.payload[4],
                                  (uint16_t)(f.payload_len - 4));
            uint32_t rx = 0, total = 0;
            uint8_t state = 0;
            Ota_Status(&state, &rx, &total);
            uint8_t resp[5] = { st, 0, 0, 0, 0 };
            resp[1] = (uint8_t)(rx & 0xFF);
            resp[2] = (uint8_t)((rx >> 8) & 0xFF);
            resp[3] = (uint8_t)((rx >> 16) & 0xFF);
            resp[4] = (uint8_t)((rx >> 24) & 0xFF);
            DataLink_SendFrame(CMD_OTA_DATA, resp, sizeof(resp));
            break;
        }

        case CMD_OTA_END: {
            uint8_t st = Ota_End();
            DataLink_SendFrame(CMD_OTA_END, &st, 1);
            break;
        }

        case CMD_OTA_STATUS: {
            uint8_t state = 0;
            uint32_t rx = 0, total = 0;
            Ota_Status(&state, &rx, &total);
            uint8_t resp[9] = { state, 0, 0, 0, 0, 0, 0, 0, 0 };
            resp[1] = (uint8_t)(rx & 0xFF);
            resp[2] = (uint8_t)((rx >> 8) & 0xFF);
            resp[3] = (uint8_t)((rx >> 16) & 0xFF);
            resp[4] = (uint8_t)((rx >> 24) & 0xFF);
            resp[5] = (uint8_t)(total & 0xFF);
            resp[6] = (uint8_t)((total >> 8) & 0xFF);
            resp[7] = (uint8_t)((total >> 16) & 0xFF);
            resp[8] = (uint8_t)((total >> 24) & 0xFF);
            DataLink_SendFrame(CMD_OTA_STATUS, resp, sizeof(resp));
            break;
        }

        case CMD_OTA_RESET: {
            uint8_t st = Ota_Reset();
            DataLink_SendFrame(CMD_OTA_RESET, &st, 1);
            break;
        }

        default:
            send_error_response(f.cmd, PROTO_ERR_UNKNOWN_CMD);
            break;
    }
}
