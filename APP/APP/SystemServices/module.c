#include "module.h"
#include "logger.h"
#include "led_app.h"
#include "key_app.h"
#include "buzzer_app.h"
#include "ota_agent.h"
#include "sysmon.h"
#include "var_manager.h"
#include "data_link.h"
#include "data_agent.h"
#include "la_sample.h"
#include "la_buffer.h"
#include "lcd_app.h"
#include "touch_svc.h"
#include "imu_svc.h"
#include "eth_app.h"

#include <string.h>

/* 模块注册表：priority 数值小者先初始化。
 * 依赖顺序：VAR -> DataLink -> LA_Buffer -> LA_Sample -> 应用模块 -> SysMon/DataAgent */
static module_desc_t module_table[] = {
    MODULE_INIT("VAR",       0,  VAR_Init),
    MODULE_INIT("DataLink", 10,  DataLink_Init),
    MODULE_INIT("LA_Buffer",20,  LA_Buffer_Init),
    MODULE_INIT("LA_Sample",30,  LA_Sample_Init),
    MODULE_INIT("KeyApp",   40,  KeyApp_Init),
    MODULE_INIT("BuzzerApp",42,  BuzzerApp_Init),
    MODULE_INIT("TouchSvc", 45,  TouchSvc_Init),
    MODULE_INIT("LedApp",   50,  LedApp_Init),
    MODULE_INIT("ImuSvc",   52,  ImuSvc_Init),
    MODULE_INIT("LcdApp",   55,  LcdApp_Init),
    MODULE_INIT("OtaAgent", 60,  OtaAgent_Init),
    MODULE_INIT("EthApp",   65,  EthApp_Init),
    MODULE_INIT("SysMon",   70,  SysMon_Init),
    MODULE_INIT("DataAgent",80,  DataAgent_Init),
};
#define MODULE_COUNT (sizeof(module_table) / sizeof(module_table[0]))

static void sort_modules(void)
{
    /* 稳定插入排序：按 priority 升序，保证依赖模块先初始化 */
    module_desc_t table[MODULE_COUNT];
    memcpy(table, module_table, sizeof(table));

    for (unsigned int i = 1; i < MODULE_COUNT; i++) {
        module_desc_t key = table[i];
        int j = (int)i - 1;
        while (j >= 0 && table[j].priority > key.priority) {
            table[j + 1] = table[j];
            j--;
        }
        table[j + 1] = key;
    }
    memcpy((void *)module_table, table, sizeof(table));
}

void modules_init(void)
{
    sort_modules();

    LOG_Printf("[APP] Module registry: %u entries\r\n", (unsigned)MODULE_COUNT);
    int count = 0;
    for (unsigned int i = 0; i < MODULE_COUNT; i++) {
        if (module_table[i].init) {
            LOG_Printf("[APP]   [%u] %-10s prio=%u\r\n", i,
                       module_table[i].name ? module_table[i].name : "?",
                       (unsigned)module_table[i].priority);
            module_table[i].init();
            count++;
        }
    }
    LOG_Printf("[APP] Modules initialized: %d\r\n", count);
}
