#ifndef OTA_AGENT_H
#define OTA_AGENT_H

#include <stdint.h>

/* OTA 状态机 */
#define OTA_ST_IDLE         0
#define OTA_ST_RECEIVING    1
#define OTA_ST_DONE         2

void    OtaAgent_Init(void);

/* HOSTLINK 命令入口（data_link 分派调用），返回 0=成功 非0=错误码 */
uint8_t Ota_Begin(uint32_t version, uint32_t size);
uint8_t Ota_Data(uint32_t offset, const uint8_t *data, uint16_t len);
uint8_t Ota_End(void);
uint8_t Ota_Status(uint8_t *state, uint32_t *received, uint32_t *total);

#endif
