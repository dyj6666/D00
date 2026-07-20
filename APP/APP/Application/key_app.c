#include "key_app.h"
#include "event_bus.h"
#include "pinout.h"
#include "app_config.h"
#include "cmsis_os2.h"          // 提供 os 相关定义，间接包含 FreeRTOS.h
#include "FreeRTOS.h"          // 提供 TimerHandle_t 等
#include "timers.h"            // 提供 xTimerCreate, xTimerStart
#include "module.h"

static void key_scan_timer_cb(TimerHandle_t xTimer) {
    static uint8_t debounce = 0;
    static uint32_t press_ms = 0;
    int state = (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == KEY0_PRESSED_STATE) ? 0 : 1;

    if (state == 0) {
        if (++debounce >= 5) {
            debounce = 5;
            press_ms += KEY_SCAN_PERIOD_MS;
            if (press_ms >= 1000) {
                MSG_SEND_SIMPLE(MODULE_KEY, MSG_KEY_LONG);
                press_ms = 0;
            }
        }
    } else {
        if (debounce >= 5) {
            if (press_ms >= 10 && press_ms < 1000) {
                MSG_SEND_SIMPLE(MODULE_KEY, MSG_KEY_SHORT);
            }
        }
        debounce = 0;
        press_ms = 0;
    }
}

void KeyApp_Init(void) {
    TimerHandle_t tmr = xTimerCreate("key", pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS),
                                     pdTRUE, NULL, key_scan_timer_cb);
    if (tmr != NULL) {
        xTimerStart(tmr, 0);
    }
}
