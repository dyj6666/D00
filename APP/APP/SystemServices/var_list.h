#ifndef VAR_LIST_H
#define VAR_LIST_H

#include <stdint.h>
#include "var_manager.h"

/* LIST_VARS 分片打包（纯逻辑，可主机测试）。
 *
 * 每包帧布局（不含 CRC）:
 *   [0..1] SYNC1/SYNC2
 *   [2]    CMD_LIST_VARS
 *   [3..4] payload_len = 分片头(2) + 条目数据
 *   [5]    total_packets
 *   [6]    packet_index (从 0 开始)
 *   [7..]  条目流: id(2) type(1) permission(1) name_len(1) name(name_len)
 */

/* 计算全部变量分片后的包数（空表返回 1，单包包含分片头）。
 * max_frame_len 为不含 CRC 的整帧长度上限。 */
uint8_t VarList_TotalPackets(const VarEntry *entries, uint16_t count,
                             uint16_t max_frame_len);

/* 构建第 packet_index 包（0 基）。frame 容量 cap。
 * 成功返回 0，*out_len 为不含 CRC 的帧长度（可直接交 DataLink_SendPacket）。 */
int VarList_BuildPacket(const VarEntry *entries, uint16_t count,
                        uint16_t max_frame_len,
                        uint8_t total_packets, uint8_t packet_index,
                        uint8_t *frame, uint16_t cap, uint16_t *out_len);

#endif
