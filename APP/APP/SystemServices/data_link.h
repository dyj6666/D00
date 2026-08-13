/* ================================================================
 * data_link —— HOSTLINK 串口协议链路接口
 *
 * 架构位置：APP 服务层；SendFrame/Wait 供业务模块上报
 * ================================================================ */
#ifndef DATA_LINK_H
#define DATA_LINK_H

#include <stdint.h>

void DataLink_Init(void);

/* 发送一帧（不含 CRC，函数内部追加 CRC）。返回 0 成功，-1 失败。 */
int DataLink_SendPacket(const uint8_t *data, uint16_t len);

/* 组装完整帧（含 CRC）并发送。返回 0 成功，-1 失败。 */
int DataLink_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len);

/* 组装完整帧并以背压方式发送（队列满时阻塞，最多 timeout_ms）。
 * 用于大块可靠导出；返回 0 成功，-1 超时/失败。 */
int DataLink_SendFrameWait(uint8_t cmd, const uint8_t *payload,
                           uint16_t payload_len, uint32_t timeout_ms);

/* 发送统一错误帧（CMD_ERROR + 原命令码 + 错误码），供命令实现方使用 */
void DataLink_SendError(uint8_t orig_cmd, uint8_t err_code);

/* OTA 命令处理器：由 OTA 模块注册（data_link 不感知 OTA 细节，避免向上依赖） */
typedef void (*data_link_ota_handler_t)(uint8_t cmd, const uint8_t *payload,
                                        uint8_t payload_len);
void DataLink_SetOtaHandler(data_link_ota_handler_t handler);

/* 命令队列溢出计数（上位机突发时丢了多少帧） */
uint32_t DataLink_GetCmdLostCount(void);

/* TX 流缓冲溢出计数（发送丢帧数） */
uint32_t DataLink_GetTxLostCount(void);

/* TX DMA 异常/超时自愈计数（正常应为 0） */
uint32_t DataLink_GetTxErrorCount(void);

#endif
