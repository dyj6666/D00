#include "led_app.h"
#include "event_bus.h"
#include "pinout.h"
#include "logger.h"
#include "module.h"
#include <string.h>
#include "var_manager.h"

static uint8_t led_state_value = 0;
static int32_t test_counter = 0;
static float test_temp = 25.5f;
static int32_t writable_var = 0;



typedef enum {
    LED_STATE_OFF,
    LED_STATE_ON,
    LED_STATE_SLOW_BLINK,
    LED_STATE_FAST_BLINK,
    LED_STATE_COUNT
} led_state_t;

static void state_off(event_id_t evt, const void *payload, uint32_t len);
static void state_on(event_id_t evt, const void *payload, uint32_t len);
static void state_slow_blink(event_id_t evt, const void *payload, uint32_t len);
static void state_fast_blink(event_id_t evt, const void *payload, uint32_t len);

typedef void (*state_handler_t)(event_id_t, const void*, uint32_t);
static const state_handler_t state_table[LED_STATE_COUNT] = {
    [LED_STATE_OFF]  = state_off,
    [LED_STATE_ON]   = state_on,
    [LED_STATE_SLOW_BLINK] = state_slow_blink,
    [LED_STATE_FAST_BLINK] = state_fast_blink,
};

static led_state_t current_state = LED_STATE_OFF;

static void led_switch_state(led_state_t new_state) {
    current_state = new_state;
    state_table[current_state](EVENT_NONE, NULL, 0);
}

static void led_event_handler(event_id_t evt, const void *payload, uint32_t len) {
    state_table[current_state](evt, payload, len);
}

static void led_cmd_handler(event_id_t evt, const void *payload, uint32_t len) {
    if (evt != EVENT_CMD_LED) return;
    const char *cmd = (const char *)payload;
    if (strcmp(cmd, "on") == 0) {
        led_switch_state(LED_STATE_ON);
    } else if (strcmp(cmd, "off") == 0) {
        led_switch_state(LED_STATE_OFF);
    } else if (strcmp(cmd, "toggle") == 0) {
        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
        LOG_Printf("LED toggled\r\n");
    } else {
        LOG_Printf("Usage: led on/off/toggle\r\n");
    }
}

/* ---------- 状态实现 ---------- */
static void state_off(event_id_t evt, const void *payload, uint32_t len) {
    (void)payload; (void)len;
    switch (evt) {
        case EVENT_NONE:
            HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, LED_OFF_STATE);
            LOG_Printf("LED: OFF\r\n");
            break;
        case EVENT_KEY_SHORT:
            led_switch_state(LED_STATE_ON);
            break;
        case EVENT_KEY_LONG:
            led_switch_state(LED_STATE_SLOW_BLINK);
            break;
        default: break;
    }
}

static void state_on(event_id_t evt, const void *payload, uint32_t len) {
    (void)payload; (void)len;
    switch (evt) {
        case EVENT_NONE:
            HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, LED_ON_STATE);
            LOG_Printf("LED: ON\r\n");
            break;
        case EVENT_KEY_SHORT:
            led_switch_state(LED_STATE_OFF);
            break;
        case EVENT_KEY_LONG:
            led_switch_state(LED_STATE_FAST_BLINK);
            break;
        default: break;
    }
}

static void state_slow_blink(event_id_t evt, const void *payload, uint32_t len) {
    (void)payload; (void)len;
    switch (evt) {
        case EVENT_NONE:
            LOG_Printf("LED: SLOW BLINK\r\n");
            break;
        case EVENT_TICK_1S:
            HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
            break;
        case EVENT_KEY_SHORT:
            led_switch_state(LED_STATE_FAST_BLINK);
            break;
        case EVENT_KEY_LONG:
            led_switch_state(LED_STATE_OFF);
            break;
        default: break;
    }
}

static void state_fast_blink(event_id_t evt, const void *payload, uint32_t len) {
    (void)payload; (void)len;
    switch (evt) {
        case EVENT_NONE:
            LOG_Printf("LED: FAST BLINK\r\n");
            break;
        case EVENT_TICK_200MS:
            HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
            break;
        case EVENT_KEY_SHORT:
            led_switch_state(LED_STATE_OFF);
            break;
        case EVENT_KEY_LONG:
            led_switch_state(LED_STATE_SLOW_BLINK);
            break;
        default: break;
    }
}

void LedApp_Init(void) {
    EventBus_Subscribe(EVENT_TICK_1S, led_event_handler);
    EventBus_Subscribe(EVENT_TICK_200MS, led_event_handler);
    EventBus_Subscribe(EVENT_KEY_SHORT, led_event_handler);
    EventBus_Subscribe(EVENT_KEY_LONG, led_event_handler);
    EventBus_Subscribe(EVENT_CMD_LED, led_cmd_handler);
    led_switch_state(LED_STATE_OFF);


    VAR_Register(0x1001, "led_state", VAR_TYPE_UINT8, 0, &led_state_value);
    VAR_Register(0x1002, "counter", VAR_TYPE_INT32, 0, &test_counter);
    VAR_Register(0x1003, "temperature", VAR_TYPE_FLOAT, 0, &test_temp);
    VAR_Register(0x2001, "writable", VAR_TYPE_INT32, 1, &writable_var);
}

// MODULE_REGISTER(LedApp_Init);
