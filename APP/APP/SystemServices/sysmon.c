/* ================================================================
 * sysmon —— 系统监控：堆/栈/任务状态周期汇总
 *
 * 架构位置：APP 服务层；sysmon 命令与 LCD 页共用
 * ================================================================ */
#include "sysmon.h"
#include "bsp.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "timers.h"
#include "task.h"
#include "logger.h"
#include "event_bus.h"
#include "FreeRTOS.h"
#include "watchdog.h"
#include "data_link.h"
#include "err_mgr.h"
#include "eth_app.h"
#include "icmp_svc.h"

#include <string.h>

/* ================== 喂狗（SysTick 中断驱动） ==================
 * 工业级原则：硬件看门狗绝不可依赖低优先级任务——事件风暴/高优先级任务
 * 长时间占用 CPU 会饿死 Tmr Svc，导致 IWDG 误复位。改为 SysTick 钩子
 * 在中断上下文喂狗，任何任务调度风暴都不影响喂狗。 */
void vApplicationTickHook(void)
{
    static uint32_t tick_cnt = 0;
    /* err_mgr 运行时长快照（SysTick 优先级 15 >= syscall 限制，ISR API 合法） */
    ERR_TickMs = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (++tick_cnt >= WDOG_FEED_PERIOD_MS) {   /* 1kHz tick => 周期=WDOG_FEED_PERIOD_MS ms */
        tick_cnt = 0;
        BSP_Watchdog_Refresh();
    }
}

/* ================== 监控项定义 ================== */
typedef void (*monitor_item_func)(void);   // 采集并直接打印

typedef struct {
    const char *name;          // 监控项名称（打印用）
    monitor_item_func print;   // 采集+打印函数
} monitor_item_t;

/* ---- 各监控项的采集打印函数 ---- */
static void print_event_bus_stats(void)
{
    LOG_Printf("=== EVENT BUS ===\r\n");
    LOG_Printf("  Lost messages: %lu\r\n", EventBus_GetLostCount());
}

static void print_data_link_stats(void)
{
    LOG_Printf("=== DATA LINK ===\r\n");
    LOG_Printf("  Cmd queue lost: %lu\r\n", DataLink_GetCmdLostCount());
    LOG_Printf("  TX frames lost: %lu\r\n", DataLink_GetTxLostCount());
    LOG_Printf("  TX errors:      %lu\r\n", DataLink_GetTxErrorCount());
}

static void print_eth_stats(void)
{
    EthApp_RefreshStatus();
    const eth_status_t *st = EthApp_GetStatus();
    LOG_Printf("=== ETH ===\r\n");
    LOG_Printf("  Link: %s", st->link_up ? "UP" : "DOWN");
    if (st->link_up) {
        LOG_Printf("  IP: %u.%u.%u.%u  MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   st->ip[0], st->ip[1], st->ip[2], st->ip[3],
                   st->mac[0], st->mac[1], st->mac[2],
                   st->mac[3], st->mac[4], st->mac[5]);
        LOG_Printf("  RX: %lu packets  TX: %lu packets  UP: %lu s\r\n",
                   (unsigned long)st->rx_packets,
                   (unsigned long)st->tx_packets,
                   (unsigned long)st->link_uptime_s);
    } else {
        LOG_Printf("\r\n");
    }
}

static void print_icmp_stats(void)
{
    const icmp_svc_stat_t *st = IcmpSvc_GetStat();
    LOG_Printf("=== ICMP ===\r\n");
    LOG_Printf("  Echo rx/tx/drop: %lu/%lu/%lu  Other rx: %lu\r\n",
               (unsigned long)st->echo_rx,
               (unsigned long)st->echo_tx,
               (unsigned long)st->echo_drop,
               (unsigned long)st->other_rx);
    LOG_Printf("  Rate: %lu pps (peak %lu)  RTT: %lu/%lu/%lu us\r\n",
               (unsigned long)st->rate_pps,
               (unsigned long)st->peak_pps,
               (unsigned long)st->min_rtt_us,
               (unsigned long)st->avg_rtt_us,
               (unsigned long)st->max_rtt_us);
}

static void print_crash_info(void)
{
    LOG_Printf("=== LAST CRASH ===\r\n");
    LOG_Printf("  Crash seq: %lu\r\n", (unsigned long)ERR_GetCrashSeq());
    const err_record_t *rec = ERR_GetLastRecord();
    if (rec != NULL) {
        LOG_Printf("  Cause: %s\r\n", rec->cause);
    }
}

static void print_task_list(void)
{
    /* 逐行输出：避免一次性打印超长字符串触发日志缓冲截断 */
    UBaseType_t size = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = pvPortMalloc(size * sizeof(TaskStatus_t));
    if (arr == NULL) {
        LOG_Printf("=== TASKS ===\r\n  (no memory)\r\n");
        return;
    }

    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(arr, size, &total);
    LOG_Printf("=== TASKS ===\r\n");
    LOG_Printf("%-16s %s %5s %6s %6s\r\n", "Name", "St", "Prio", "Stack", "#");
    for (UBaseType_t i = 0; i < n; i++) {
        char state = '?';
        switch (arr[i].eCurrentState) {
            case eRunning:   state = 'X'; break;
            case eReady:     state = 'R'; break;
            case eBlocked:   state = 'B'; break;
            case eSuspended: state = 'S'; break;
            case eDeleted:   state = 'D'; break;
            default:         state = '?'; break;
        }
        LOG_Printf("%-16s %c %5u %6u %6u\r\n",
                   arr[i].pcTaskName, state,
                   (unsigned)arr[i].uxCurrentPriority,
                   (unsigned)arr[i].usStackHighWaterMark,
                   (unsigned)arr[i].xTaskNumber);
    }
    vPortFree(arr);
}

static void print_cpu_usage(void)
{
    /* 1 秒窗口差分算法：
     * DWT 周期计数器在 168 MHz 下 32 位约 25.6 s 回绕，直接对“启动以来累加值”
     * 求百分比会得到垃圾数据；改为对相邻两次快照做差分，窗口远小于回绕周期，
     * 数值稳定且不受回绕影响。 */
    UBaseType_t size = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap1 = pvPortMalloc(size * sizeof(TaskStatus_t));
    TaskStatus_t *snap2 = pvPortMalloc(size * sizeof(TaskStatus_t));
    uint32_t total1 = 0, total2 = 0;

    if (snap1 == NULL || snap2 == NULL) {
        vPortFree(snap1);
        vPortFree(snap2);
        LOG_Printf("=== CPU USAGE ===\r\n  (no memory)\r\n");
        return;
    }

    UBaseType_t n1 = uxTaskGetSystemState(snap1, size, &total1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    UBaseType_t n2 = uxTaskGetSystemState(snap2, size, &total2);

    uint32_t dt = total2 - total1;
    if (dt == 0) dt = 1;

    LOG_Printf("=== CPU USAGE (last 1s) ===\r\n");
    for (UBaseType_t i = 0; i < n2; i++) {
        uint32_t base = 0;
        for (UBaseType_t j = 0; j < n1; j++) {
            if (strcmp(snap1[j].pcTaskName, snap2[i].pcTaskName) == 0) {
                base = snap1[j].ulRunTimeCounter;
                break;
            }
        }
        uint32_t delta = snap2[i].ulRunTimeCounter - base;
        uint32_t pct = (uint32_t)(((uint64_t)delta * 100) / dt);
        if (pct > 100) pct = 100;
        LOG_Printf("  %-16s %3lu%%\r\n", snap2[i].pcTaskName, (unsigned long)pct);
    }

    vPortFree(snap1);
    vPortFree(snap2);
}

static void print_heap_info(void)
{
    LOG_Printf("=== HEAP ===\r\nFree heap: %lu bytes\r\n", (unsigned long)xPortGetFreeHeapSize());
}

static void print_watchdog_status(void)
{
    LOG_Printf("=== WATCHDOG ===\r\nIWDG active, feed via SysTick IRQ every %d ms\r\n",
               WDOG_FEED_PERIOD_MS);
#if APP_DEBUG_MODE
    LOG_Printf("  (debug mode: watchdog disabled)\r\n");
#endif
    WDOG_PrintStatus();
}

static void print_reset_reason(void)
{
    bsp_reset_reason_t reason = BSP_GetResetReason();
    LOG_Printf("=== RESET REASON ===\r\n");
    switch (reason) {
        case BSP_RESET_IWDG:     LOG_Printf("  Independent watchdog reset\r\n"); break;
        case BSP_RESET_WWDG:     LOG_Printf("  Window watchdog reset\r\n");     break;
        case BSP_RESET_POWER_ON: LOG_Printf("  Power-on reset\r\n");            break;
        case BSP_RESET_PIN:      LOG_Printf("  External pin reset\r\n");        break;
        case BSP_RESET_SOFTWARE: LOG_Printf("  Software reset\r\n");            break;
        default:                 LOG_Printf("  Unknown reset reason\r\n");      break;
    }
}

/* ---- 监控项注册表（添加新监控只需在这里加一行） ---- */
static const monitor_item_t monitor_items[] = {
    {"Tasks",       print_task_list},
    {"CPU Usage",   print_cpu_usage},
    {"Heap",        print_heap_info},
    {"Watchdog",    print_watchdog_status},
    {"Reset Reason",print_reset_reason},
      {"Event Bus",   print_event_bus_stats},
      {"DataLink",    print_data_link_stats},
      {"ETH",         print_eth_stats},
      {"ICMP",        print_icmp_stats},
      {"Last Crash",  print_crash_info},
    // 示例：未来添加监控变量
    // {"Custom Sensor", print_custom_sensor},
};
#define MONITOR_ITEM_COUNT (sizeof(monitor_items) / sizeof(monitor_items[0]))

/* ================== 事件处理：收到 sysmon 请求时打印所有监控项 ================== */
static void handle_sysmon_msg(const message_t *msg)
{
    if (msg == NULL) return;
    if (msg->hdr.type != MSG_CMD_SYSMON) return;

    LOG_Printf("\r\n===== SYSTEM MONITOR =====\r\n");
    for (size_t i = 0; i < MONITOR_ITEM_COUNT; i++) {
        if (monitor_items[i].print) {
            monitor_items[i].print();
            /* 节间退让：UART@115200 排水 ~11.5KB/s，不加延时整段输出会
             * 撑满 2KB LOG 流缓冲导致中段被截断（sysmon 曾只显示首行） */
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    LOG_Printf("===========================\r\n");
}

/* ================== 模块初始化 ================== */
void SysMon_Init(void)
{
    /* 错误管理初始化 + 启动时复现上次崩溃原因 */
    ERR_Init();
    ERR_ReportLastCrash();

#if APP_DEBUG_MODE
    LOG_Printf("SysMon: debug mode, watchdog disabled.\r\n");
#else
    /* 启动任务级看门狗（IWDG 之外的软件层防线） */
    WDOG_Init();

    LOG_Printf("[APP] SysMon: online, IWDG feed via SysTick (%d ms)\r\n",
               WDOG_FEED_PERIOD_MS);
#endif

    // 2. 订阅 sysmon 命令事件（使用新消息类型）
    EventBus_Subscribe(MSG_CMD_SYSMON, handle_sysmon_msg);

}
