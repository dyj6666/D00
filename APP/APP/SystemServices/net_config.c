/* ================================================================
 * 网络配置持久化实现（日志式 NVM，见 net_config.h）
 * ================================================================ */
#include "net_config.h"
#include "bsp_nvm.h"
#include "crc16.h"
#include "app_config.h"
#include "logger.h"

#include <string.h>

/* 槽位 32B：magic(4) seq(4) ip(4) mask(4) gw(4) crc(2) rsv(10) */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint16_t crc;
    uint8_t  rsv[10];
} nv_cfg_entry_t;

#define NET_CFG_SLOT_SIZE   (sizeof(nv_cfg_entry_t))          /* 32 */
#define NET_CFG_SLOT_COUNT  (NET_CFG_NVM_SIZE / NET_CFG_SLOT_SIZE)
/* CRC 覆盖 magic..gw 共 20 字节（不含 crc 字段自身） */
#define NET_CFG_CRC_LEN     20

static net_cfg_t s_saved;
static uint8_t  s_have = 0;

static uint16_t entry_crc(const nv_cfg_entry_t *e)
{
    return CRC16_Calculate((const uint8_t *)e, NET_CFG_CRC_LEN);
}

static void entry_read(nv_cfg_entry_t *e, uint32_t idx)
{
    BSP_NVM_ReadWords(NET_CFG_NVM_BASE + idx * NET_CFG_SLOT_SIZE,
                      (uint32_t *)e, NET_CFG_SLOT_SIZE / 4u);
}

static int entry_valid(const nv_cfg_entry_t *e)
{
    return (e->magic == NET_CFG_NVM_MAGIC) && (entry_crc(e) == e->crc);
}

void NvConfig_Init(void)
{
    BSP_NVM_Init();
    s_have = 0;
    for (uint32_t i = 0; i < NET_CFG_SLOT_COUNT; i++) {
        nv_cfg_entry_t e;
        entry_read(&e, i);
        if (!entry_valid(&e)) {
            continue;
        }
        if (e.ip[0] == 0u && e.ip[1] == 0u && e.ip[2] == 0u && e.ip[3] == 0u) {
            s_have = 0;                   /* 显式清除（net ip default） */
        } else {
            memcpy(&s_saved, &e.ip, sizeof(s_saved));
            s_have = 1;
        }
    }
    LOG_Printf("[NET] NVM cfg: %s\r\n",
               s_have ? "saved IP, applied on boot" : "default");
}

int NvConfig_Load(net_cfg_t *cfg)
{
    if (!s_have) {
        return 0;
    }
    if (cfg != NULL) {
        memcpy(cfg, &s_saved, sizeof(*cfg));
    }
    return 1;
}

/* 日志满时的维护：保留 BOOT 参数双槽（0x080E0000 / +1KB），整扇区擦除后重建 */
static int nvm_maintain(const nv_cfg_entry_t *ne)
{
    uint32_t boot0[8], boot1[8];
    BSP_NVM_ReadWords(OTA_PARAM_ADDR, boot0, 8);
    BSP_NVM_ReadWords(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET, boot1, 8);

    if (BSP_NVM_EraseParamSector() != 0) {
        return -4;
    }
    if (BSP_NVM_ProgramWords(OTA_PARAM_ADDR, boot0, 8) != 0 ||
        BSP_NVM_ProgramWords(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                             boot1, 8) != 0) {
        return -5;
    }
    uint32_t words[NET_CFG_SLOT_SIZE / 4u];
    memcpy(words, ne, sizeof(*ne));
    return BSP_NVM_ProgramWords(NET_CFG_NVM_BASE, words,
                                NET_CFG_SLOT_SIZE / 4u);
}

int NvConfig_Save(const net_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }

    nv_cfg_entry_t last;
    int have_last = 0;
    uint32_t free_idx = NET_CFG_SLOT_COUNT;
    for (uint32_t i = 0; i < NET_CFG_SLOT_COUNT; i++) {
        nv_cfg_entry_t e;
        entry_read(&e, i);
        if (entry_valid(&e)) {
            last = e;
            have_last = 1;
            continue;
        }
        free_idx = i;                    /* 第一个空/坏槽 */
        break;
    }

    nv_cfg_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.magic = NET_CFG_NVM_MAGIC;
    ne.seq = have_last ? last.seq + 1u : 1u;
    memcpy(ne.ip, cfg->ip, 4);
    memcpy(ne.mask, cfg->mask, 4);
    memcpy(ne.gw, cfg->gw, 4);
    ne.crc = entry_crc(&ne);

    int r;
    if (free_idx >= NET_CFG_SLOT_COUNT) {
        r = nvm_maintain(&ne);
    } else {
        uint32_t words[NET_CFG_SLOT_SIZE / 4u];
        memcpy(words, &ne, sizeof(ne));
        r = BSP_NVM_ProgramWords(NET_CFG_NVM_BASE + free_idx * NET_CFG_SLOT_SIZE,
                                 words, NET_CFG_SLOT_SIZE / 4u);
    }
    if (r == 0) {
        if (ne.ip[0] == 0u && ne.ip[1] == 0u && ne.ip[2] == 0u && ne.ip[3] == 0u) {
            s_have = 0;
        } else {
            memcpy(&s_saved, &ne.ip, sizeof(s_saved));
            s_have = 1;
        }
    }
    return r;
}

int NvConfig_Clear(void)
{
    net_cfg_t z;
    memset(&z, 0, sizeof(z));
    return NvConfig_Save(&z);
}
