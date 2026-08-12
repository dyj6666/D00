/* ================================================================
 * cmd_can —— CAN Shell 传输适配器实现
 *
 * 架构位置：APP 服务层；物理层经 BSP_CAN 收发，行帧规约见 can_proto.h
 *
 * 数据流：
 *   RX：BSP 回调(0x100) → 按序拼行 → Cmd_SessionFeed 分发（命令执行
 *       期间 LOG_Printf 自动路由回本适配器，输出零改动）；
 *   TX：ctx.out → 整段切帧（seq 递增，末帧 0x80）→ BSP_CAN_Send(0x101)。
 * ================================================================ */
#include "cmd_can.h"
#include "cmd_shell.h"
#include "can_proto.h"
#include "bsp_can.h"
#include "logger.h"
#include "app_config.h"

#include <string.h>

#if CMD_ENABLE_CAN

static cmd_session_t s_can_session;              /* 行会话（复用统一命令框架） */
static uint8_t  s_rx_buf[CMD_LINE_MAX];          /* 行组装缓冲 */
static uint16_t s_rx_len;
static uint8_t  s_rx_seq;                        /* 期望的下一帧序号 */
static uint8_t  s_registered;

/** @brief 输出回调：把命令输出整段切成行帧组（seq 递增，末帧置 0x80） */
static void can_shell_out(cmd_ctx_t *ctx, const char *s, uint16_t len)
{
    (void)ctx;
    uint16_t off = 0;
    uint8_t seq = 0;
    do {
        uint8_t frame[CAN_FRAME_DLC];
        uint8_t n = 0;
        while (n < CAN_FRAME_PAYLOAD && off < len) {
            frame[1 + n] = (uint8_t)s[off];
            off++;
            n++;
        }
        frame[0] = seq | ((off >= len) ? CAN_FRAME_LAST : 0u);
        if (BSP_CAN_Send(CAN_SHELL_TX_ID, frame, (uint8_t)(n + 1)) != 0) {
            return;                              /* 队列满：剩余输出截断 */
        }
        seq = (seq + 1) & CAN_FRAME_SEQ_MASK;
    } while (off < len);
}

/** @brief RX 回调（BSP canRx 任务上下文）：0x100 行帧组按序拼行并分发 */
static void can_shell_rx(uint32_t id, const uint8_t *data, uint8_t dlc, void *ctx)
{
    (void)ctx;
    if (id != CAN_SHELL_RX_ID || dlc == 0) {
        return;
    }
    uint8_t seq = data[0] & CAN_FRAME_SEQ_MASK;
    uint8_t last = data[0] & CAN_FRAME_LAST;
    if (seq == 0) {
        s_rx_len = 0;                            /* 新行组开始 */
        s_rx_seq = 0;
    }
    if (seq != s_rx_seq) {
        s_rx_len = 0;                            /* 乱序：整组丢弃防串行 */
        s_rx_seq = 0;
        return;
    }
    uint8_t n = (uint8_t)(dlc - 1);
    if ((uint16_t)(s_rx_len + n) >= CMD_LINE_MAX) {
        s_rx_len = 0;                            /* 超长行：丢弃重来 */
        s_rx_seq = 0;
        return;
    }
    memcpy(&s_rx_buf[s_rx_len], &data[1], n);
    s_rx_len += n;
    s_rx_seq++;
    if (last) {
        if (s_rx_len > 0) {
            uint32_t dispatched = Cmd_SessionFeed(&s_can_session,
                                                  s_rx_buf, s_rx_len);
            if (dispatched > 0) {
                static const char prompt[] = "\r\n" CMD_PROMPT;
                can_shell_out(&s_can_session.ctx, prompt,
                              (uint16_t)(sizeof(prompt) - 1));
            }
        }
        s_rx_len = 0;
        s_rx_seq = 0;
    }
}

static const cmd_transport_t s_can_transport = {
    .name  = "CAN",
    .mask  = CMD_TRANSPORT_CAN,
    .start = NULL,   /* 硬件/任务由 BSP_CAN_Init 自管，注册即就绪 */
};

void CmdCan_Register(void)
{
    if (s_registered) {
        return;                                  /* 幂等 */
    }
    Cmd_SessionReset(&s_can_session, CMD_TRANSPORT_CAN, NULL, can_shell_out);
    if (BSP_CAN_RegisterRxCb(can_shell_rx, NULL) == 0) {
        Cmd_TransportRegister(&s_can_transport);
        s_registered = 1;
        LOG_Printf("[CAN] shell transport ready (RX 0x%03X / TX 0x%03X)\r\n",
                   (unsigned)CAN_SHELL_RX_ID, (unsigned)CAN_SHELL_TX_ID);
    }
}

#else

void CmdCan_Register(void)
{
    /* CAN 未启用：空实现，保持接入点存在 */
}

#endif
