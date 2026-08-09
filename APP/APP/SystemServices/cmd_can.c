/* ================================================================
 * CAN 传输适配器（模板 / 未来扩展）
 *
 * 启用步骤（架构保证"一个适配器文件 + 一行注册"即可接入）：
 *   1. app_config.h 或构建定义 CMD_ENABLE_CAN=1；
 *   2. 实现 CAN 硬件接收 → 行组装（可直接复用 Cmd_SessionFeed）：
 *        - 每个 CAN 数据帧按行协议填充 cmd_session_t，
 *          收到行尾（\n）后由 Cmd_SessionFeed 自动分发；
 *   3. ctx.out 负责把命令输出按 CAN 帧回传；
 *   4. Cmd_TransportRegister(&s_can_transport)（本文件已就绪）；
 *   5. 在模块初始化表（module.c）调用 CmdCan_Register()。
 * 命令实现零改动（cmd_catalog 与传输无关）。
 * ================================================================ */
#include "cmd_shell.h"
#include "cmd_can.h"

#if CMD_ENABLE_CAN

/* CAN 接收任务/硬件初始化（接入驱动后实现） */
static void cmd_can_start(void)
{
    /* TODO: 创建 CAN RX 任务，帧 → 行 → Cmd_SessionFeed */
}

static const cmd_transport_t s_can_transport = {
    .name  = "CAN",
    .mask  = CMD_TRANSPORT_CAN,
    .start = cmd_can_start,
};

void CmdCan_Register(void)
{
    Cmd_TransportRegister(&s_can_transport);
}

#else

void CmdCan_Register(void)
{
    /* CAN 未启用：空实现，保持接入点存在 */
}

#endif
