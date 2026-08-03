#ifndef DATA_LINK_H
#define DATA_LINK_H

#include <stdint.h>

void DataLink_Init(void);

/* 发送一帧（不含 CRC，函数内部追加 CRC）。返回 0 成功，-1 失败。 */
int DataLink_SendPacket(const uint8_t *data, uint16_t len);

/* 组装完整帧（含 CRC）并发送。返回 0 成功，-1 失败。 */
int DataLink_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len);

/* 命令队列溢出计数（上位机突发时丢了多少帧） */
uint32_t DataLink_GetCmdLostCount(void);

#endif
