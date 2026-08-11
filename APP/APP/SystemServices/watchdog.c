/* ================================================================
 * watchdog —— 任务级看门狗：心跳监控 + 超时复位
 *
 * 架构位置：APP 服务层；与硬件 IWDG 双层防线
 * ================================================================ */
#include "watchdog.h"
#include "bsp.h"
#include "logger.h"
#include "cmsis_os2.h"
#include "err_mgr.h"

#include <string.h>

typedef struct {
    const char *name;
    TaskHandle_t handle;
    volatile uint32_t last_kick_tick;
    uint32_t timeout_ticks;
    uint8_t  used;
} wdg_entry_t;

static wdg_entry_t g_wdg[WDOG_MAX_TASKS];
static TaskHandle_t wdg_monitor_handle = NULL;
static volatile uint8_t wdg_inited = 0;

#define WDOG_MONITOR_PERIOD_MS 500

static void wdg_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WDOG_MONITOR_PERIOD_MS));

        uint32_t now = xTaskGetTickCount();
        taskENTER_CRITICAL();
        for (int i = 0; i < WDOG_MAX_TASKS; i++) {
            if (!g_wdg[i].used) continue;
            uint32_t silent = now - g_wdg[i].last_kick_tick;
            if (silent > g_wdg[i].timeout_ticks) {
                taskEXIT_CRITICAL();
                /* 进入统一错误管理（转储 + BKP 持久化 + 软复位） */
                ERR_HandleTaskStall(g_wdg[i].name ? g_wdg[i].name : "?",
                                    silent * portTICK_PERIOD_MS);
                for (;;) {}   /* 理论上不可达（ERR 内部复位） */
            }
        }
        taskEXIT_CRITICAL();
    }
}

void WDOG_Init(void)
{
    osThreadAttr_t attr = {
        .name = "WDOG",
        .stack_size = 512,
        .priority = osPriorityLow,
    };

    if (wdg_inited) return;
    memset(g_wdg, 0, sizeof(g_wdg));

    wdg_monitor_handle = (TaskHandle_t)osThreadNew(wdg_monitor_task, NULL, &attr);
    if (wdg_monitor_handle != NULL) {
        wdg_inited = 1;
    }
}

int WDOG_RegisterTask(const char *name, TaskHandle_t handle, uint32_t timeout_ms)
{
    if (handle == NULL || timeout_ms == 0) return -1;
    if (!wdg_inited) WDOG_Init();   /* 懒初始化：允许任务先于 SysMon 运行 */
    if (!wdg_inited) return -2;

    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) timeout_ticks = 1;

    taskENTER_CRITICAL();
    int slot = -1;
    for (int i = 0; i < WDOG_MAX_TASKS; i++) {
        if (g_wdg[i].used && g_wdg[i].handle == handle) {
            slot = i;
            break;
        }
        if (!g_wdg[i].used && slot < 0) slot = i;
    }
    int ret = 0;
    if (slot < 0) {
        ret = -3;   /* 表满 */
    } else {
        g_wdg[slot].name = name;
        g_wdg[slot].handle = handle;
        g_wdg[slot].timeout_ticks = timeout_ticks;
        g_wdg[slot].last_kick_tick = xTaskGetTickCount();
        g_wdg[slot].used = 1;
    }
    taskEXIT_CRITICAL();
    return ret;
}

void WDOG_Kick(TaskHandle_t handle)
{
    if (handle == NULL) return;
    taskENTER_CRITICAL();
    for (int i = 0; i < WDOG_MAX_TASKS; i++) {
        if (g_wdg[i].used && g_wdg[i].handle == handle) {
            g_wdg[i].last_kick_tick = xTaskGetTickCount();
            break;
        }
    }
    taskEXIT_CRITICAL();
}

void WDOG_PrintStatus(void)
{
    uint32_t now = xTaskGetTickCount();
    uint32_t silent_ms[WDOG_MAX_TASKS];
    uint32_t timeout_ms[WDOG_MAX_TASKS];
    const char *names[WDOG_MAX_TASKS];

    taskENTER_CRITICAL();
    for (int i = 0; i < WDOG_MAX_TASKS; i++) {
        if (!g_wdg[i].used) continue;
        uint32_t silent = now - g_wdg[i].last_kick_tick;
        silent_ms[i] = silent * portTICK_PERIOD_MS;
        timeout_ms[i] = g_wdg[i].timeout_ticks * portTICK_PERIOD_MS;
        names[i] = g_wdg[i].name ? g_wdg[i].name : "?";
    }
    taskEXIT_CRITICAL();

    LOG_Printf("=== TASK WATCHDOG ===\r\n");
    for (int i = 0; i < WDOG_MAX_TASKS; i++) {
        if (!g_wdg[i].used) continue;
        LOG_Printf("  %-12s silent=%lu ms / timeout=%lu ms\r\n",
                   names[i], (unsigned long)silent_ms[i], (unsigned long)timeout_ms[i]);
    }
}
