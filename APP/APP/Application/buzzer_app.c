/* ================================================================
 * buzzer_app —— 蜂鸣器应用：提示音/告警音控制
 *
 * 架构位置：APP 应用层；交互反馈
 * ================================================================ */
#include "buzzer_app.h"
#include "bsp_buzzer.h"
#include "bsp.h"
#include "event_bus.h"
#include "FreeRTOS.h"
#include "timers.h"

/* ================================================================
 * 蜂鸣时序状态机：Tmr Svc 回调按"响/停"序列逐段驱动。
 * 序列以 [on0,gap0,on1,gap1,...] 表示，段间 gap 播停、末段后自然结束。
 * 所有调用均非阻塞（立即返回，时长由定时器控制）。
 * ================================================================ */

#define BUZZER_SEQ_MAX   8   /* 最多 8 段（OTA 旋律最多 4 段，余量充足） */

typedef struct {
    TimerHandle_t timer;
    uint16_t seq[BUZZER_SEQ_MAX];  /* on/gap 交替 */
    uint8_t  len;                  /* 段数 */
    uint8_t  idx;                  /* 当前段 */
    uint8_t  phase;                /* 0=响 1=间隙 */
} buzzer_seq_t;

static buzzer_seq_t s_bz;

static void buzzer_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_bz.phase == 0) {
        /* 响结束：关蜂鸣；若还有下一段则进入段间间隙 */
        BSP_Buzzer_Off();
        if (s_bz.idx + 1u < s_bz.len) {
            uint16_t gap = s_bz.seq[s_bz.idx * 2u + 1u];
            s_bz.phase = 1;
            xTimerChangePeriod(s_bz.timer,
                               pdMS_TO_TICKS((gap > 0) ? gap : 1u), 0);
        }
    } else {
        /* 间隙结束：进入下一段响 */
        s_bz.idx++;
        if (s_bz.idx < s_bz.len) {
            uint16_t on = s_bz.seq[s_bz.idx * 2u];
            BSP_Buzzer_On();
            s_bz.phase = 0;
            xTimerChangePeriod(s_bz.timer,
                               pdMS_TO_TICKS((on > 0) ? on : 1u), 0);
        }
    }
}

void Buzzer_Beep(uint16_t ms)
{
    uint16_t seq[2] = { ms, 1 };
    Buzzer_PlaySequence(seq, 1);
}

void Buzzer_BeepPattern(uint8_t count, uint16_t on_ms, uint16_t gap_ms)
{
    uint16_t seq[BUZZER_SEQ_MAX * 2u];
    uint8_t n = (count > BUZZER_SEQ_MAX) ? BUZZER_SEQ_MAX
                                          : ((count > 0) ? count : 1);
    for (uint8_t i = 0; i < n; i++) {
        seq[i * 2u] = (on_ms > 0) ? on_ms : 10;
        seq[i * 2u + 1u] = gap_ms;
    }
    Buzzer_PlaySequence(seq, n);
}

/**
 * @brief  播放任意节奏序列（非阻塞）
 * @param  on_gap  数组：[on0,gap0,on1,gap1,...]，on/gap 单位 ms
 * @param  n       段数
 */
void Buzzer_PlaySequence(const uint16_t *on_gap, uint8_t n)
{
    if (s_bz.timer == NULL || on_gap == NULL || n == 0) return;
    if (n > BUZZER_SEQ_MAX) n = BUZZER_SEQ_MAX;

    xTimerStop(s_bz.timer, 0);        /* 取消进行中的序列 */
    /* 关键：序列是 on/gap 交替的 2n 项，必须全部拷贝；
     * 若只拷 n 项，后续 gap 会读到静态零值，触发
     * xTimerChangePeriod(0) 的 FreeRTOS 断言（timers.c 836 行）。 */
    for (uint8_t i = 0; i < n * 2u; i++) {
        s_bz.seq[i] = (on_gap[i] > 0) ? on_gap[i] : 1;
    }
    s_bz.len = n;
    s_bz.idx = 0;
    s_bz.phase = 0;
    BSP_Buzzer_On();
    xTimerChangePeriod(s_bz.timer, pdMS_TO_TICKS(s_bz.seq[0]), 0);
}

void Buzzer_Stop(void)
{
    if (s_bz.timer == NULL) return;
    xTimerStop(s_bz.timer, 0);
    BSP_Buzzer_Off();
}

/* ---------- OTA 旋律（有源蜂鸣器：节奏即音高表达） ---------- */
/**
 * @brief  阻塞式播放节奏序列（OTA 专用，不依赖 Tmr Svc 调度）
 * @param  on_gap  [on0,gap0,on1,gap1,...]，单位 ms
 * @param  n       段数
 * @note   阻塞总时长 ≤1s；从任务上下文调用，期间暂停该任务（可接受）
 */
static void buzzer_ota_block(const uint16_t *on_gap, uint8_t n)
{
    if (on_gap == NULL || n == 0) return;
    Buzzer_Stop();   /* 停掉可能残留的非阻塞序列 */
    for (uint8_t i = 0; i < n; i++) {
        uint16_t on = (on_gap[i * 2u] > 0) ? on_gap[i * 2u] : 1;
        BSP_Buzzer_On();
        BSP_DelayMs(on);
        BSP_Buzzer_Off();
        if (i + 1u < n) {
            BSP_DelayMs(on_gap[i * 2u + 1u]);
        }
    }
}

/** @brief 升级开始/下载就绪：滴-滴-嘟（两短一长，上行收束感） */
void Buzzer_OtaStart(void)
{
    static const uint16_t seq[] = { 80, 50, 80, 50, 160, 0 };
    buzzer_ota_block(seq, 3);
}

/** @brief 下载完成：滴-滴（双短音，收尾感，随后触发 BOOT 切换） */
void Buzzer_OtaDownloadDone(void)
{
    static const uint16_t seq[] = { 70, 60, 70, 0 };
    buzzer_ota_block(seq, 2);
}

/** @brief 升级成功（新固件启动确认）：滴-滴-滴-嘟（三短一长） */
void Buzzer_OtaSuccess(void)
{
    static const uint16_t seq[] = { 70, 50, 70, 50, 70, 50, 220, 0 };
    buzzer_ota_block(seq, 4);
}

/** @brief 升级失败：滴-滴-滴（三短等间隔，温和警示） */
void Buzzer_OtaFail(void)
{
    static const uint16_t seq[] = { 60, 60, 60, 60, 60, 0 };
    buzzer_ota_block(seq, 3);
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
