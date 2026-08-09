/* ================================================================
 * 网络配置持久化（EEPROM 用户存储，USR_KEY_NET_CFG）
 *   - `net ip <a.b.c.d>` 保存到 EEPROM，上电自动应用上次配置；
 *   - 属用户数据：统一走 usr_store（日志式 EEPROM），
 *     OTA 参数仍在 flash（PARAM 区），两者互不干扰；
 *   - 由 EthApp 在启动时 NetConfig_Init + NetConfig_Load。
 * ================================================================ */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdint.h>

typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
} net_cfg_t;

/* 初始化：从 EEPROM 加载最新配置（幂等，须在 usr_store 之后） */
void NetConfig_Init(void);

/* 读取保存的配置；返回 1=有且有效（写入 cfg），0=无/已被清除 */
int  NetConfig_Load(net_cfg_t *cfg);

/* 保存到 EEPROM（日志追加）；返回 0=成功，负=失败 */
int  NetConfig_Save(const net_cfg_t *cfg);

/* 清除保存配置（写墓碑，等价 net ip default） */
int  NetConfig_Clear(void);

#endif
