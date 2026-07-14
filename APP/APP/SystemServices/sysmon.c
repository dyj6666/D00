#include "sysmon.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "timers.h"
#include "task.h"
#include "logger.h"
#include "main.h"           // 提供 HAL_StatusTypeDef 等
#include "event_bus.h"
#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"  // 提供 HAL_IWDG_Refresh 声明
#include <stdio.h>
#include <string.h>

/* ================== 喂狗定时器 ================== */
static TimerHandle_t feed_timer;
extern IWDG_HandleTypeDef hiwdg;

static void feed_callback(TimerHandle_t xTimer)
{
    HAL_IWDG_Refresh(&hiwdg);
}

/* ================== 监控项定义 ================== */
typedef void (*monitor_item_func)(void);   // 采集并直接打印

typedef struct {
    const char *name;          // 监控项名称（打印用）
    monitor_item_func print;   // 采集+打印函数
} monitor_item_t;

/* ---- 各监控项的采集打印函数 ---- */
static void print_task_list(void)
{
    char buf[512];
    vTaskList(buf);
    LOG_Printf("=== TASKS ===\r\n%s\r\n", buf);
}

static void print_cpu_usage(void)
{
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize;
    uint32_t ulTotalRunTime;

    uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));
    if (pxTaskStatusArray) {
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);
        ulTotalRunTime /= 100UL;
        if (ulTotalRunTime == 0) ulTotalRunTime = 1;

        LOG_Printf("=== CPU USAGE ===\r\n");
        for (UBaseType_t x = 0; x < uxArraySize; x++) {
            uint32_t ulPercent = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalRunTime;
            LOG_Printf("  %-16s %3lu%%\r\n", pxTaskStatusArray[x].pcTaskName, ulPercent);
        }
        vPortFree(pxTaskStatusArray);
    }
}

static void print_heap_info(void)
{
    LOG_Printf("=== HEAP ===\r\nFree heap: %lu bytes\r\n", xPortGetFreeHeapSize());
}

static void print_watchdog_status(void)
{
    LOG_Printf("=== WATCHDOG ===\r\nIWDG active, feed period: %d ms\r\n", WDOG_FEED_PERIOD_MS);
}

static void print_reset_reason(void)
{
    uint32_t flags = RCC->CSR;
    LOG_Printf("=== RESET REASON ===\r\n");
    if (flags & RCC_CSR_IWDGRSTF)  LOG_Printf("  Independent watchdog reset\r\n");
    if (flags & RCC_CSR_WWDGRSTF)  LOG_Printf("  Window watchdog reset\r\n");
    if (flags & RCC_CSR_PORRSTF)   LOG_Printf("  Power-on reset\r\n");
    if (flags & RCC_CSR_PINRSTF)   LOG_Printf("  External pin reset\r\n");
    if (flags & RCC_CSR_SFTRSTF)   LOG_Printf("  Software reset\r\n");
    RCC->CSR |= RCC_CSR_RMVF;
}

/* ---- 监控项注册表（添加新监控只需在这里加一行） ---- */
static const monitor_item_t monitor_items[] = {
    {"Tasks",       print_task_list},
    {"CPU Usage",   print_cpu_usage},
    {"Heap",        print_heap_info},
    {"Watchdog",    print_watchdog_status},
    {"Reset Reason",print_reset_reason},
    // 示例：未来添加监控变量
    // {"Custom Sensor", print_custom_sensor},
};
#define MONITOR_ITEM_COUNT (sizeof(monitor_items) / sizeof(monitor_items[0]))

/* ================== 事件处理：收到 sysmon 请求时打印所有监控项 ================== */
static void handle_sysmon_request(event_id_t evt, const void *payload, uint32_t len)
{
    (void)payload; (void)len;
    if (evt != EVENT_CMD_SYSMON) return;

    LOG_Printf("\r\n===== SYSTEM MONITOR =====\r\n");
    for (size_t i = 0; i < MONITOR_ITEM_COUNT; i++) {
        if (monitor_items[i].print) {
            monitor_items[i].print();
        }
    }
    LOG_Printf("===========================\r\n");
}

/* ================== 模块初始化 ================== */
void SysMon_Init(void)
{
    // 1. 启动喂狗定时器
    feed_timer = xTimerCreate("wdg_feed",
                              pdMS_TO_TICKS(WDOG_FEED_PERIOD_MS),
                              pdTRUE, NULL, feed_callback);
    if (feed_timer) {
        xTimerStart(feed_timer, 0);
        LOG_Printf("SysMon: IWDG feeding started.\r\n");
    }

    // 2. 订阅 sysmon 命令事件
    EventBus_Subscribe(EVENT_CMD_SYSMON, handle_sysmon_request);

    LOG_Printf("SysMon: Online.\r\n");
}