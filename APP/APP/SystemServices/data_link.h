#ifndef DATA_LINK_H
#define DATA_LINK_H
#include <stdint.h>

void DataLink_Init(void);
void DataLink_SendPacket(const uint8_t *data, uint16_t len);
void DataLink_RxIdleCallback(uint16_t size);
void DataLink_TxCpltCallback(void);
#endif