/* ================================================================
 * SNTP 时间同步服务（RFC4330 最小客户端，UDP:123）
 *   - `sntp sync [server]`：同步并写入 RTC（本地时区 UTC+8）
 *   - `sntp auto on|off`：周期自动同步（默认开，1 小时，上电 5s 后首同步）
 *   - 服务器地址持久化到 EEPROM（用户数据，USR_KEY_SNTP_SERVER）
 * ================================================================ */
#ifndef SNTP_SVC_H
#define SNTP_SVC_H

#include <stdint.h>

void SntpSvc_Init(void);
int  SntpSvc_SetServer(const char *ip);            /* 0=成功（写 EEPROM + 生效） */
const uint8_t *SntpSvc_GetServer(void);            /* NULL=未配置 */
int  SntpSvc_Sync(const uint8_t server[4], uint32_t timeout_ms); /* 0=成功并写 RTC */
int  SntpSvc_SetAuto(uint8_t on);
uint8_t SntpSvc_Auto(void);
void SntpSvc_GetTimeStr(char *buf, uint32_t len);  /* "YYYY-MM-DD HH:MM:SS" */

#endif
