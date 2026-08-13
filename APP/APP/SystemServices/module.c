/* ================================================================
 * module —— 模块注册表：按优先级稳定排序并顺序初始化
 *
 * 架构位置：APP 服务层；StartupTask 调用 modules_init() 一次性拉起全部模块
 * 核心流程：静态模块表 -> 插入排序(priority 升序) -> 逐个 init()
 * 关键约束：依赖模块优先级更低；初始化失败不中断后续模块
 * ================================================================ */
#include "module.h"
#include "logger.h"
#include "cmd_catalog.h"
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
#include "tcp_svc.h"
#include "icmp_svc.h"
#include "dns_svc.h"
#include "sntp_svc.h"
#include "mqtt_svc.h"
#include "http_svc.h"
#include "ota_transport.h"
#include "ota_tcp_svc.h"
#include "cmd_shell.h"
#include "shell.h"
#include "usr_store.h"
#include "bsp_can.h"
#include "cmd_can.h"
#include "ota_can_svc.h"

#include <string.h>

/* 模块注册表：priority 数值小者先初始化。
 * 依赖顺序：VAR -> DataLink -> LA_Buffer -> LA_Sample -> 应用模块 -> SysMon/DataAgent */
static module_desc_t module_table[] = {
MODULE_INIT("Cmd",    2,  Cmd_Init),
MODULE_INIT("CmdCat", 3,  CmdCatalog_Register),
MODULE_INIT("CanBsp", 3,  BSP_CAN_Init),
    MODULE_INIT("USR",    3,  UsrStore_Init),
    MODULE_INIT("Shell",  4,  Shell_Init),
    MODULE_INIT("CmdCan", 5,  CmdCan_Register),
    MODULE_INIT("DNS",    5,  DnsSvc_Init),
    MODULE_INIT("SNTP",   6,  SntpSvc_Init),
    MODULE_INIT("OtaMgr", 7,  OtaMgr_Init),
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
    MODULE_INIT("IcmpSvc",  66,  IcmpSvc_Init),
    MODULE_INIT("TcpSvc",   67,  TcpSvc_Init),
    MODULE_INIT("MqttSvc",  68,  MqttSvc_Init),
    MODULE_INIT("HttpSvc",  69,  HttpSvc_Init),
    MODULE_INIT("SysMon",   70,  SysMon_Init),
    MODULE_INIT("OtaTcp",   71,  OtaTcpSvc_Init),
    MODULE_INIT("OtaCan",   72,  OtaCanSvc_Init),
    MODULE_INIT("DataAgent",80,  DataAgent_Init),
};
#define MODULE_COUNT (sizeof(module_table) / sizeof(module_table[0]))

/** @brief 稳定插入排序：按 priority 升序，保证依赖模块先于依赖者初始化 */
static void sort_modules(void)
{
    module_desc_t table[MODULE_COUNT];
    memcpy(table, module_table, sizeof(table));   /* 工作副本，避免原地覆盖 */

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

/** @brief 按优先级初始化全部模块（打印注册表供启动日志核验） */
void modules_init(void)
{
    sort_modules();   /* 先排序，再顺序 init */

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
