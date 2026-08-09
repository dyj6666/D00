#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdint.h>

/* ================================================================
 * 网络配置持久化（PARAM 区日志式 NVM）
 *   - 每次保存向日志区追加一个 32B 槽位（只写不擦，免整扇区频繁擦除）
 *   - 上电扫描取"最后一个有效条目"= 上次保存的配置
 *   - 日志满（128 次）执行维护：保留 BOOT 参数双槽后整扇区重建
 * ================================================================ */

typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
} net_cfg_t;

/* 初始化：扫描日志缓存最新有效配置（幂等） */
void NvConfig_Init(void);

/* 读取保存的配置；返回 1=有且有效（写入 cfg），0=无/已被清除 */
int  NvConfig_Load(net_cfg_t *cfg);

/* 追加保存；返回 0=成功，负=失败 */
int  NvConfig_Save(const net_cfg_t *cfg);

/* 清除保存配置（写"用默认"标记条目，等价 net ip default） */
int  NvConfig_Clear(void);

#endif
