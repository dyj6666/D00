/* ================================================================
 * 用户数据统一存储（日志式 EEPROM，板载 AT24C02 256B）
 *   - 所有需要掉电保持的"用户数据"一律经本服务存取（触摸校准、
 *     网络配置、未来设备参数）；OTA 参数仍留在 flash，互不干扰；
 *   - 布局（256B 顺序日志，追加写，免擦除、天然分散磨损）：
 *       记录  = magic(1)=0xA5 key(1) len(1) crc16(2) data[len]
 *       删除  = 追加 len=0xFF 墓碑记录（该 key 最新状态即"无"）
 *       空间不足 → 整片紧凑（compact）：收集各 key 最新值，
 *                 整片写 0xFF 后按 key 顺序重建日志；
 *   - 读 = 单遍扫描取各 key 最新有效记录，CRC16 全程校验；
 *   - 内部互斥串行化，上层无需关心介质细节。
 * ================================================================ */
#ifndef USR_STORE_H
#define USR_STORE_H

#include <stdint.h>

/* ---- 键注册表：新增用户数据只需在这里登记一个键 ---- */
#define USR_KEY_TOUCH_CAL   1   /* bsp_touch_cal_t（17B） */
#define USR_KEY_NET_CFG     2   /* net_cfg_t（12B） */
#define USR_KEY_DNS_SERVER  3   /* uint8_t[4] */
#define USR_KEY_SNTP_SERVER 4   /* uint8_t[4] */
#define USR_KEY_MQTT_BROKER 5   /* uint8_t[4]+port(2) */

#define USR_EEPROM_SIZE     256
#define USR_DATA_MAX        200  /* 单记录数据上限（还受剩余空间约束） */

void UsrStore_Init(void);                       /* 探测 + 日志校验（幂等） */
int  UsrStore_Valid(void);                      /* 1=EEPROM 在线且日志有效 */
int  UsrStore_Get(uint8_t key, void *out, uint16_t max_len); /* 返回 len；<0 无/错 */
int  UsrStore_Set(uint8_t key, const void *data, uint16_t len); /* 0=成功 */
int  UsrStore_Erase(uint8_t key);               /* 追加墓碑 */
int  UsrStore_Reset(void);                      /* 整片擦除重格式化 */
uint32_t UsrStore_Count(void);                  /* 有效 key 数 */
void UsrStore_Info(uint32_t *used, uint32_t *free); /* 日志占用/空闲字节 */

#endif
