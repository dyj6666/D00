/* ================================================================
 * 网络配置持久化实现（EEPROM 用户存储，见 net_config.h）
 * ================================================================ */
#include "net_config.h"
#include "usr_store.h"
#include "logger.h"

#include <string.h>

static net_cfg_t s_saved;
static uint8_t  s_have = 0;

void NetConfig_Init(void)
{
    s_have = 0;
    memset(&s_saved, 0, sizeof(s_saved));
    if (UsrStore_Get(USR_KEY_NET_CFG, &s_saved, sizeof(s_saved)) ==
        (int)sizeof(s_saved)) {
        s_have = 1;
    }
    LOG_Printf("[NET] cfg: %s\r\n",
               s_have ? "saved IP, applied on boot" : "default");
}

int NetConfig_Load(net_cfg_t *cfg)
{
    if (!s_have) {
        return 0;
    }
    if (cfg != NULL) {
        memcpy(cfg, &s_saved, sizeof(*cfg));
    }
    return 1;
}

int NetConfig_Save(const net_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }
    int r = UsrStore_Set(USR_KEY_NET_CFG, cfg, sizeof(*cfg));
    if (r == 0) {
        memcpy(&s_saved, cfg, sizeof(s_saved));
        s_have = 1;
        LOG_Printf("[NET] cfg saved to EEPROM\r\n");
    } else {
        LOG_Printf("[NET] WARN cfg save to EEPROM failed (%d)\r\n", r);
    }
    return r;
}

int NetConfig_Clear(void)
{
    int r = UsrStore_Erase(USR_KEY_NET_CFG);
    if (r == 0) {
        s_have = 0;
        memset(&s_saved, 0, sizeof(s_saved));
    }
    return r;
}
