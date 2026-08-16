/* ================================================================
 * led_app —— LED 应用：开关/闪烁/告警模式状态机
 *
 * 架构位置：APP 应用层；状态指示与故障告警
 * ================================================================ */
#include "led_app.h"
#include "bsp.h"
#include "event_bus.h"
#include "logger.h"
#include "var_manager.h"
#include "var_ids.h"
#include <string.h>

/* ---------- 可暴露给上位机的变量 ---------- */
static uint8_t led_state_value = 0;   /* LED 当前状态值（LED_STATE_*） */
static int32_t writable_var = 0;      /* 可写变量演示 */

/* ---------- LED 状态枚举 ---------- */
typedef enum {
    LED_STATE_OFF,
    LED_STATE_ON,
    LED_STATE_SLOW_BLINK,
    LED_STATE_FAST_BLINK,
    LED_STATE_COUNT
} led_state_t;

/* ---------- 状态函数声明 ---------- */
static void state_off(const message_t *msg);
static void state_on(const message_t *msg);
static void state_slow_blink(const message_t *msg);
static void state_fast_blink(const message_t *msg);

/* ---------- 状态函数表 ---------- */
typedef void (*state_handler_t)(const message_t *msg);
static const state_handler_t state_table[LED_STATE_COUNT] = {
    [LED_STATE_OFF]         = state_off,
    [LED_STATE_ON]          = state_on,
    [LED_STATE_SLOW_BLINK]  = state_slow_blink,
    [LED_STATE_FAST_BLINK]  = state_fast_blink,
};

static led_state_t current_state = LED_STATE_OFF;

/* ---------- 内部辅助函数 ---------- */
static void led_switch_state(led_state_t new_state)
{
    current_state = new_state;
    led_state_value = (uint8_t)new_state;   /* 同步上位机变量 */
    /* 进入新状态时调用一次，msg == NULL 表示初始化动作 */
    state_table[current_state](NULL);
}

/* ---------- 统一消息处理回调（订阅入口） ---------- */
static void led_msg_handler(const message_t *msg)
{
    if (msg == NULL) return;

    /* LED 专用命令直接处理，不经过状态机 */
    if (msg->hdr.type == MSG_CMD_LED) {
        const char *cmd = (const char *)msg->payload;
        if (strcmp(cmd, "on") == 0) {
            led_switch_state(LED_STATE_ON);
        } else if (strcmp(cmd, "off") == 0) {
            led_switch_state(LED_STATE_OFF);
        } else if (strcmp(cmd, "toggle") == 0) {
            BSP_LED_Toggle(0);
            LOG_Printf("LED toggled\r\n");
        } else {
            LOG_Printf("Usage: led on/off/toggle\r\n");
        }
        return;
    }

    /* 其他事件分发给当前状态处理 */
    state_table[current_state](msg);
}

/* ---------- 状态实现 ---------- */
static void state_off(const message_t *msg)
{
    if (msg == NULL) {
        /* 进入状态 */
        BSP_LED_Set(0, 0);
        LOG_Printf("[APP] LED  : OFF\r\n");
        return;
    }

    switch (msg->hdr.type) {
        default:
            break;
    }
}

static void state_on(const message_t *msg)
{
    if (msg == NULL) {
        BSP_LED_Set(0, 1);
        LOG_Printf("[APP] LED  : ON\r\n");
        return;
    }

    switch (msg->hdr.type) {
        default:
            break;
    }
}

static void state_slow_blink(const message_t *msg)
{
    if (msg == NULL) {
        LOG_Printf("[APP] LED  : SLOW BLINK\r\n");
        return;
    }

    switch (msg->hdr.type) {
        case MSG_TICK_1S:
            BSP_LED_Toggle(0);
            break;
        default:
            break;
    }
}

static void state_fast_blink(const message_t *msg)
{
    if (msg == NULL) {
        LOG_Printf("[APP] LED  : FAST BLINK\r\n");
        return;
    }

    switch (msg->hdr.type) {
        case MSG_TICK_200MS:
            BSP_LED_Toggle(0);
            break;
        default:
            break;
    }
}

/* ---------- 模块初始化 ---------- */
void LedApp_Init(void)
{
    EventBus_Subscribe(MSG_TICK_1S,     led_msg_handler);
    EventBus_Subscribe(MSG_TICK_200MS,  led_msg_handler);
    EventBus_Subscribe(MSG_CMD_LED,     led_msg_handler);

    led_switch_state(LED_STATE_OFF);

    VAR_Register(VAR_ID_LED_STATE, "led_state", VAR_TYPE_UINT8, 0, &led_state_value);
    VAR_Register(VAR_ID_WRITABLE,  "writable",  VAR_TYPE_INT32, 1, &writable_var);
}
