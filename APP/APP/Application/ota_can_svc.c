/* ================================================================
 * ota_can_svc —— CAN OTA 服务实现
 *
 * 架构位置：APP 应用层；仅做"CAN 帧 ↔ OTA 下载核心"的协议翻译
 *
 * 关键约束：
 *   - 下载核心（Ota_Begin/Data/End）带互斥与断点续传，传输层零侵入；
 *   - 数据流严格顺序：乱序/超长整组丢弃，防总线故障污染固件包；
 *   - 无数据超时（5s）自动 ABORT，避免下载区悬挂占用；
 *   - 应答帧 0x210 供上位机确认每一阶段（BEGIN/END/STATUS）。
 * ================================================================ */
#include "ota_can_svc.h"
#include "ota_agent.h"
#include "ota_transport.h"
#include "bsp_can.h"
#include "can_proto.h"
#include "event_bus.h"
#include "logger.h"
#include "app_config.h"

#include "stm32f4xx_hal.h"

#include <string.h>

#define OTA_CAN_IDLE_TIMEOUT_MS  5000u   /* 无数据超时自动中止 */

/* 会话状态 */
static uint8_t  s_active;                    /* BEGIN 成功后置位 */
static uint8_t  s_stream[OTA_CHUNK_MAX];     /* 240B 块组装缓冲 */
static uint16_t s_stream_len;
static uint8_t  s_stream_seq;                /* 期望帧序号 */
static uint32_t s_received;                  /* 已写入下载区的字节数 */
static uint32_t s_last_rx_tick;              /* 最后收到帧的时刻 */

/* 诊断计数（复测后清理） */
static uint32_t s_dbg_seq_err;   /* 诊断：乱序丢弃计数（限流打印） */

/** @brief 应答发送辅助：单帧（≤8B） */
static void ota_reply(const uint8_t *data, uint8_t dlc)
{
    (void)BSP_CAN_Send(CAN_OTA_REPLY_ID, data, dlc);
}

/** @brief 块 ACK 发送：携带已收字节数（主机据此发下一块） */
static void ota_block_ack(uint32_t received, uint8_t ok)
{
    uint8_t f[5];
    f[0] = ok ? CAN_OTA_ACK_OK : CAN_OTA_ACK_ERR;
    memcpy(&f[1], &received, 4);
    (void)BSP_CAN_Send(CAN_OTA_ACK_ID, f, 5);
}

/** @brief 小端 32 位读取（CAN 负载无对齐约束） */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief 控制帧处理：BEGIN/END/STATUS/ABORT */
static void ota_can_ctrl(const uint8_t *d, uint8_t dlc)
{
    uint8_t rep[8];
    if (dlc < 1) {
        return;
    }
    switch (d[0]) {
    case CAN_OTA_CMD_BEGIN: {                  /* BEGIN：version + size */
        if (dlc < 8) {
            rep[0] = CAN_OTA_REP_BEGIN_ERR;
            rep[1] = 2;                        /* 参数非法 */
            ota_reply(rep, 2);
            break;
        }
        /* 8B 载荷：size LE32 + version LE16 + 保留字节（单帧装得下） */
        uint32_t size = le32(&d[1]);
        uint32_t ver = (uint32_t)d[5] | ((uint32_t)d[6] << 8);
        s_received = 0;
        s_stream_len = 0;
        s_stream_seq = 0;
        uint8_t rc = Ota_Begin(ver, size);
        if (rc == 0) {
            /* 断点续传：对齐已收字节，避免重写已写区 */
            uint8_t st;
            uint32_t total = 0;
            Ota_Status(&st, &s_received, &total);
            s_active = 1;
            s_last_rx_tick = HAL_GetTick();
            rep[0] = CAN_OTA_REP_BEGIN_OK;
            rep[1] = 0;
        } else {
            s_active = 0;
            rep[0] = CAN_OTA_REP_BEGIN_ERR;
            rep[1] = rc;
        }
        ota_reply(rep, 2);
        LOG_Printf("[OTA-CAN] begin v=%lu size=%lu rc=%u\r\n",
                   (unsigned long)ver, (unsigned long)size, (unsigned)rc);
        break;
    }
    case CAN_OTA_CMD_END: {                    /* END：收尾并触发切换 */
        uint8_t rc = 1;
        if (s_active) {
            /* 冲刷末尾不足一块的余量（仅允许在最终块出现） */
            if (s_stream_len > 0) {
                uint8_t w = Ota_Data(s_received, s_stream, s_stream_len);
                if (w == 0) {
                    s_received += s_stream_len;
                }
                s_stream_len = 0;
            }
            rc = Ota_End();
            if (rc != 0) {
                /* 数据未收齐：保持会话活动，等待主机补齐后重发 END */
            } else {
                s_active = 0;
            }
        }
        rep[0] = CAN_OTA_REP_END_RESULT;
        rep[1] = rc;
        ota_reply(rep, 2);
        LOG_Printf("[OTA-CAN] end rc=%u\r\n", (unsigned)rc);
        break;
    }
    case CAN_OTA_CMD_STATUS: {                 /* STATUS：状态 + 进度 */
        uint8_t st;
        uint32_t rx = 0, total = 0;
        Ota_Status(&st, &rx, &total);
        rep[0] = CAN_OTA_REP_STATUS;
        rep[1] = st;
        memcpy(&rep[2], &rx, 4);
        ota_reply(rep, 6);
        rep[0] = CAN_OTA_REP_STATUS_TOTAL;
        memcpy(&rep[1], &total, 4);
        ota_reply(rep, 5);
        break;
    }
    case CAN_OTA_CMD_ABORT: {                  /* ABORT：清会话回 IDLE */
        uint8_t rc = Ota_Reset();
        s_active = 0;
        s_stream_len = 0;
        s_stream_seq = 0;
        rep[0] = CAN_OTA_REP_ABORT;
        rep[1] = rc;
        ota_reply(rep, 2);
        break;
    }
    default:
        break;
    }
}

/** @brief 数据帧处理：行帧规约拼 240B 块 → Ota_Data */
static void ota_can_stream(const uint8_t *d, uint8_t dlc)
{
    if (dlc == 0) {
        return;
    }
    uint8_t seq = d[0] & CAN_FRAME_SEQ_MASK;
    uint8_t last = d[0] & CAN_FRAME_LAST;
    if (seq == 0) {
        s_stream_len = 0;                      /* 新块开始 */
        s_stream_seq = 0;
    }
    if (seq != s_stream_seq) {
        s_stream_len = 0;                      /* 乱序：整块丢弃 */
        s_stream_seq = 0;
        s_dbg_seq_err++;
        if (s_dbg_seq_err <= 3u) {
            LOG_Printf("[OTA-CAN] dbg seq err got=%u exp=%u\r\n",
                       (unsigned)seq, (unsigned)s_stream_seq);
        }
        return;
    }
    uint8_t n = (uint8_t)(dlc - 1);
    if ((uint16_t)(s_stream_len + n) > OTA_CHUNK_MAX) {
        s_stream_len = 0;                      /* 超长块：丢弃 */
        s_stream_seq = 0;
        s_dbg_seq_err++;
        if (s_dbg_seq_err <= 3u) {
            LOG_Printf("[OTA-CAN] dbg len err len=%u\r\n", (unsigned)s_stream_len);
        }
        return;
    }
    memcpy(&s_stream[s_stream_len], &d[1], n);
    s_stream_len += n;
    s_stream_seq++;
    if (last) {
        /* 整块到齐（仅最终余量块可 <240B）→ 写下载区 */
        uint8_t rc = Ota_Data(s_received, s_stream, s_stream_len);
        if (rc == 0) {
            s_received += s_stream_len;
            ota_block_ack(s_received, 1);
        } else if (rc == 2) {
            /* 块可能已被写入（主机 ACK 丢失后重传）：对齐进度并回 ACK，
             * 保证重传幂等，不会因 offset 失配级联失败 */
            uint8_t st;
            uint32_t total = 0;
            Ota_Status(&st, &s_received, &total);
            ota_block_ack(s_received, 1);
        } else {
            ota_block_ack(s_received, 0);
        }
        s_stream_len = 0;
        s_stream_seq = 0;
    }
}

/** @brief BSP RX 回调（canRx 任务上下文）：按 ID 分流控制/数据帧
 * 关键：数据流必须严格限定 0x201——总线上的应答回显帧（0x210/0x101）若
 * 被当作数据帧会以 data[0]=0x84 等伪序号污染流状态，导致后续块全部丢弃。 */
static void ota_can_rx(uint32_t id, const uint8_t *data, uint8_t dlc, void *ctx)
{
    (void)ctx;
    if (id != CAN_OTA_CTRL_ID && id != CAN_OTA_DATA_ID) {
        return;
    }
    s_last_rx_tick = HAL_GetTick();
    if (id == CAN_OTA_CTRL_ID) {
        ota_can_ctrl(data, dlc);
    } else if (id == CAN_OTA_DATA_ID && s_active) {
        ota_can_stream(data, dlc);
    }
}

/** @brief 监管（事件总线 1s 心跳）：OTA 会话无数据超时自动 ABORT。
 *  不单独建任务——并入 eventBusTask 的 MSG_TICK_1S，省一个任务与 512B 栈。 */
static void ota_can_supervise_tick(const message_t *msg)
{
    (void)msg;
    if (s_active && (HAL_GetTick() - s_last_rx_tick) > OTA_CAN_IDLE_TIMEOUT_MS) {
        LOG_Printf("[OTA-CAN] idle timeout, aborting session\r\n");
        (void)Ota_Reset();
        s_active = 0;
        s_stream_len = 0;
        s_stream_seq = 0;
    }
}

/** @brief 注册 CAN 传输（available=1）并挂接 RX 回调与 1s 监管 */
void OtaCanSvc_Init(void)
{
    static const ota_transport_t can = {
        OTA_TRANSPORT_CAN, "CAN", "CAN bus 1Mbps (ID 0x200/0x201)", 1,
    };
    if (OtaMgr_Register(&can) != 0) {
        return;                                /* 已注册（幂等） */
    }
    if (BSP_CAN_RegisterRxCb(ota_can_rx, NULL) == 0) {
        EventBus_Subscribe(MSG_TICK_1S, ota_can_supervise_tick);
        LOG_Printf("[OTA] CAN transport ready (ctrl 0x%03X data 0x%03X)\r\n",
                   (unsigned)CAN_OTA_CTRL_ID, (unsigned)CAN_OTA_DATA_ID);
    }
}
