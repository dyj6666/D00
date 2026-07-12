#include "module.h"
#include "logger.h"
#include "led_app.h"
#include "key_app.h"
#include "ota_agent.h"

/* 绝对定位到 0x08050000，占用 16 字节（两个函数指针） */
const module_desc_t __attribute__((at(0x08050000))) module_table[] = {
    REGISTER_MODULE(LedApp_Init),
    REGISTER_MODULE(KeyApp_Init),
    REGISTER_MODULE(OtaAgent_Init),
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




