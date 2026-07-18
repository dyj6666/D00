#include "module.h"
#include "logger.h"
#include "led_app.h"
#include "key_app.h"
#include "ota_agent.h"
#include "sysmon.h"
#include "var_manager.h"
#include "data_link.h"
#include "data_agent.h"
// 去掉 section 属性，改回普通定义
const module_desc_t module_table[] = {
    REGISTER_MODULE(VAR_Init),
    REGISTER_MODULE(LedApp_Init),
    REGISTER_MODULE(KeyApp_Init),
    REGISTER_MODULE(OtaAgent_Init),
    REGISTER_MODULE(SysMon_Init),
    REGISTER_MODULE(DataLink_Init),
    REGISTER_MODULE(DataAgent_Init),
};
#define MODULE_COUNT (sizeof(module_table) / sizeof(module_table[0]))

void modules_init(void)
{
    int count = 0;
    LOG_Printf("Module registry at 0x0804FFE0, %u entries.\r\n", MODULE_COUNT);
    for (unsigned int i = 0; i < MODULE_COUNT; i++) {
        if (module_table[i].init) {
            LOG_Printf("  Init %u: %p\r\n", i, (void*)module_table[i].init);
            module_table[i].init();
            count++;
        }
    }
    LOG_Printf("Modules initialized: %d\r\n", count);
}




