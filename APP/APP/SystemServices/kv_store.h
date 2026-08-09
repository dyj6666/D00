#ifndef KV_STORE_H
#define KV_STORE_H

#include <stdint.h>

/* ================================================================
 * EEPROM 键值存储（板载 AT24C02，256B）
 *   布局：0x00 头(16B：magic 'KVS1' + version) + 10 槽 × 24B
 *         槽 = key(1) + len(1) + crc16(2) + data(≤20)
 *   EEPROM 支持任意字节就地改写 → 槽位更新无需擦除，天然分散磨损
 *   上层用法：KV_Init → KV_Get/KV_Set（如触摸校准、未来设备参数）
 * ================================================================ */

#define KV_KEY_TOUCH_CAL   1      /* bsp_touch_cal_t（17B） */

void KV_Init(void);                                   /* 幂等：探测+头校验 */
int  KV_Valid(void);                                  /* 1=EEPROM 在线且头有效 */
int  KV_Get(uint8_t key, void *out, uint16_t max_len);/* 返回数据长度，-1 无 */
int  KV_Set(uint8_t key, const void *data, uint16_t len); /* ≤20；0=成功 */
int  KV_Erase(uint8_t key);
int  KV_Reset(void);                                  /* 全片重格式化 */
uint32_t KV_Count(void);

#endif
