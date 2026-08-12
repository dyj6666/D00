/* ================================================================
 * cmd_can —— CAN Shell 传输适配器
 *
 * 架构位置：APP 服务层；CMD_ENABLE_CAN=1 时启用
 * 核心流程：0x100 行帧组 → Cmd_SessionFeed → 命令输出按 0x101 行帧回传
 * 帧规约：见 Config/can_proto.h（与 D00Term 严格对齐）
 * ================================================================ */
#ifndef CMD_CAN_H
#define CMD_CAN_H

/** @brief 注册 CAN Shell 传输（幂等）：会话初始化 + RX 回调挂接 */
void CmdCan_Register(void);

#endif
