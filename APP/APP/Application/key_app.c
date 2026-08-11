/* ================================================================
 * key_app —— 按键应用：周期扫描/消抖/事件分发
 *
 * 架构位置：APP 应用层；事件经事件总线广播
 * ================================================================ */
#include "key_app.h"
#include "bsp.h"
#include "event_bus.h"
#include "app_config.h"
#include "FreeRTOS.h"
#include "timers.h"

/* 按键扫描定时器回调：软件消抖 + 短按/长按识别 */
static void key_scan_timer_cb(TimerHandle_t xTimer)
{
    static uint8_t debounce = 0;
    static uint32_t press_ms = 0;
    int state = !BSP_KeyPressed();

    if (state == 0) {
        /* 按下：连续采样满 5 次视为有效，累计按下时长 */
        if (++debounce >= 5) {
            debounce = 5;
            press_ms += KEY_SCAN_PERIOD_MS;
            if (press_ms >= 1000) {
                MSG_SEND_SIMPLE(MODULE_KEY, MSG_KEY_LONG);
                press_ms = 0;
            }
        }
    } else {
        /* 松开：若曾有效按下且时长落在短按区间，发布短按事件 */
        if (debounce >= 5 && press_ms >= 10 && press_ms < 1000) {
            MSG_SEND_SIMPLE(MODULE_KEY, MSG_KEY_SHORT);
        }
        debounce = 0;
        press_ms = 0;
    }
}

void KeyApp_Init(void)
{
    TimerHandle_t tmr = xTimerCreate("key", pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS),
                                     pdTRUE, NULL, key_scan_timer_cb);
    if (tmr != NULL) {
        xTimerStart(tmr, 0);
    }
}
