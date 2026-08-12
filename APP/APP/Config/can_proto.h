/* ================================================================
 * can_proto —— CAN 行帧协议常量（Shell / OTA 共用唯一事实源）
 *
 * 架构位置：APP 配置层；上位机 D00Term / ota_can_cli 与固件严格对齐
 *
 * 帧规约（与 HOST/D00Term/d00term.py 一致）：
 *   - 每帧数据场 ≤8B；data[0]=序号（bit7=0x80 置位表示末帧），
 *     data[1..] 为负载切片（≤7B）；
 *   - 一条逻辑行 = 一组帧（seq 从 0 递增，末帧置 0x80）；
 *   - 接收端按序拼接，乱序/超长整组丢弃。
 * ================================================================ */
#ifndef CAN_PROTO_H
#define CAN_PROTO_H

#include <stdint.h>

#define CAN_FRAME_DLC       8   /* CAN 标准帧数据场上限 */
#define CAN_FRAME_PAYLOAD   7   /* 每帧负载（data[1..]） */
#define CAN_FRAME_LAST      0x80u  /* data[0] 末帧标志 */
#define CAN_FRAME_SEQ_MASK  0x7Fu  /* data[0] 序号掩码 */

/* Shell 通道：与固件 cmd_can.c / D00Term 对称 */
#define CAN_SHELL_RX_ID     0x100u  /* 主机 → 设备：命令行 */
#define CAN_SHELL_TX_ID     0x101u  /* 设备 → 主机：命令输出 */

/* OTA 通道：与 ota_can_svc.c / ota_can_cli.py 对称 */
#define CAN_OTA_CTRL_ID     0x200u  /* 主机 → 设备：控制帧（BEGIN/END/STATUS/ABORT） */
#define CAN_OTA_DATA_ID     0x201u  /* 主机 → 设备：固件数据流（行帧规约） */
#define CAN_OTA_REPLY_ID    0x210u  /* 设备 → 主机：应答帧 */
#define CAN_OTA_ACK_ID      0x211u  /* 设备 → 主机：块写入 ACK（逐块背压） */

/* 自测帧 ID：`can test` 突发帧（loopback/总线监听验证） */
#define CAN_TEST_ID         0x300u

/* OTA 控制帧命令码（data[0]） */
#define CAN_OTA_CMD_BEGIN   0x01u   /* BEGIN：data[1..4]=size LE32, data[5..6]=version LE16, data[7]=0 */
#define CAN_OTA_CMD_END     0x02u   /* END：结束并触发 BOOT 切换 */
#define CAN_OTA_CMD_STATUS  0x03u   /* STATUS：查询当前状态 */
#define CAN_OTA_CMD_ABORT   0x04u   /* ABORT：清会话回 IDLE */

/* OTA 应答帧命令码（设备 → 主机，ID 0x210） */
#define CAN_OTA_REP_BEGIN_OK     0x81u  /* data[1]=0 */
#define CAN_OTA_REP_BEGIN_ERR    0x82u  /* data[1]=Ota_Begin 错误码 */
#define CAN_OTA_REP_END_RESULT   0x83u  /* data[1]=Ota_End 错误码（0=成功，随后复位） */
#define CAN_OTA_REP_STATUS       0x84u  /* data[1]=state, data[2..5]=received LE32 */
#define CAN_OTA_REP_STATUS_TOTAL 0x85u  /* data[1..4]=total LE32（与 0x84 成对） */
#define CAN_OTA_REP_ABORT        0x86u  /* data[1]=Ota_Reset 结果 */

/* 块 ACK 帧（ID 0x211）：data[0]=0x81 成功/0x82 失败，data[1..4]=已收字节 LE32 */
#define CAN_OTA_ACK_OK      0x81u
#define CAN_OTA_ACK_ERR     0x82u

#endif /* CAN_PROTO_H */
