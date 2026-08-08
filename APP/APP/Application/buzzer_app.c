#include "buzzer_app.h"
#include "bsp_buzzer.h"
#include "event_bus.h"
#include "FreeRTOS.h"
#include "timers.h"

/* ================================================================
 * 蜂鸣时序状态机：Tmr Svc 回调逐段驱动 响→间隙→响...
 * 所有调用均非阻塞（立即返回，时长由定时器控制）。
 * ================================================================ */

typedef struct {
    TimerHandle_t timer;
    uint8_t  phase;        /* 0=响 1=间隙 */
    uint8_t  count;        /* 剩余响次数 */
    uint16_t on_ms;
    uint16_t gap_ms;
} buzzer_seq_t;

static buzzer_seq_t s_bz;

static void buzzer_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_bz.phase == 0) {
        /* 响结束：关蜂鸣，若有后续则进入间隙 */
        BSP_Buzzer_Off();
        s_bz.phase = 1;
        if (s_bz.count > 1) {
            xTimerChangePeriod(s_bz.timer, pdMS_TO_TICKS(s_bz.gap_ms), 0);
        }
    } else {
        /* 间隙结束：下一响 */
        s_bz.count--;
        if (s_bz.count > 0) {
            BSP_Buzzer_On();
            s_bz.phase = 0;
            xTimerChangePeriod(s_bz.timer, pdMS_TO_TICKS(s_bz.on_ms), 0);
        }
    }
}

void Buzzer_Beep(uint16_t ms)
{
    Buzzer_BeepPattern(1, ms, 0);
}

void Buzzer_BeepPattern(uint8_t count, uint16_t on_ms, uint16_t gap_ms)
{
    if (s_bz.timer == NULL) return;

    xTimerStop(s_bz.timer, 0);        /* 取消进行中的序列 */
    s_bz.count = (count > 0) ? count : 1;
    s_bz.on_ms = (on_ms > 0) ? on_ms : 10;
    s_bz.gap_ms = gap_ms;
    s_bz.phase = 0;
    BSP_Buzzer_On();
    xTimerChangePeriod(s_bz.timer, pdMS_TO_TICKS(s_bz.on_ms), 0);
}

void Buzzer_Stop(void)
{
    if (s_bz.timer == NULL) return;
    xTimerStop(s_bz.timer, 0);
    BSP_Buzzer_Off();
}

/* ---------- 事件反馈 ---------- */
static void buzzer_on_event(const message_t *msg)
{
    if (msg == NULL) return;
    if (msg->hdr.type == MSG_KEY_SHORT) {
        Buzzer_Beep(25);
    } else if (msg->hdr.type == MSG_KEY_LONG) {
        Buzzer_Beep(120);
    }
}

void BuzzerApp_Init(void)
{
    BSP_Buzzer_Init();
    s_bz.timer = xTimerCreate("bz", pdMS_TO_TICKS(50), pdFALSE, NULL,
                              buzzer_timer_cb);
    if (s_bz.timer == NULL) return;   /* 创建失败：仅驱动可用 */

    EventBus_Subscribe(MSG_KEY_SHORT, buzzer_on_event);
    EventBus_Subscribe(MSG_KEY_LONG, buzzer_on_event);
}
