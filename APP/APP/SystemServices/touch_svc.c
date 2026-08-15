/* ================================================================
 * touch_svc —— 触摸服务：扫描/校准/坐标上报
 *
 * 架构位置：APP 服务层；独立触摸任务
 * ================================================================ */
#include "touch_svc.h"
#include "bsp_touch.h"
#include "bsp_lcd.h"
#include "usr_store.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "logger.h"

#include <stdio.h>

/* ================================================================
 * 触摸服务实现
 * ================================================================ */

#define TSV_TASK_STACK   1024   /* GCC 浮点标定运算/打印栈深更大，512B 偏薄 */
#define TSV_IDLE_MS      8    /* 空闲探测周期（XY 有效性判定，无需 PEN） */
#define TSV_SAMPLE_MS    2    /* 按下采样节拍（500Hz，兼顾系统负载） */
#define TSV_PROBE_N      2    /* 按下消抖：连续 N 次有效判定为按下 */
#define TOUCH_MAX_STEP   20   /* 单采样位移钳制（px）：人手最快 ~12px/采样，
                               * 超限视为噪声尖峰并钳制（保留方向） */

static touch_svc_state_t s_state;   /* 共享状态 */
static osThreadId_t s_task = NULL;

/* ---------- 采样任务 ---------- */
static void touch_svc_task(void *arg)
{
    (void)arg;
    uint8_t probe = 0;
    for (;;) {
        int32_t rx, ry;
        if (!BSP_Touch_ReadRaw(&rx, &ry)) {
            /* 无有效触点 */
            if (s_state.state == TOUCH_EVT_DOWN ||
                s_state.state == TOUCH_EVT_MOVE) {
                /* 首次抬起：发 UP（仅一次） */
                s_state.up_x = s_state.x;
                s_state.up_y = s_state.y;
                s_state.state = TOUCH_EVT_UP;
                s_state.gen++;
            }
            /* UP 状态保持到下一次按下（UI 按代数消费，不会重复触发；
             * 若立即复位 NONE 且无代数变化，UI 可能错过 UP 导致手势丢失） */
            probe = 0;
            vTaskDelay(pdMS_TO_TICKS(TSV_IDLE_MS));
            continue;
        }

        if (s_state.state == TOUCH_EVT_NONE || s_state.state == TOUCH_EVT_UP) {
            /* 空闲消抖：连续 TSV_PROBE_N 次有效才判定按下 */
            if (++probe < TSV_PROBE_N) {
                vTaskDelay(pdMS_TO_TICKS(TSV_IDLE_MS));
                continue;
            }
            probe = 0;
        } else {
            probe = 0;
        }

        {
            uint16_t lx, ly;
            BSP_Touch_Convert(rx, ry, &lx, &ly);

            /* 事件转移：NONE/UP → DOWN，其余（DOWN/MOVE）→ MOVE */
            uint8_t evt = (s_state.state == TOUCH_EVT_NONE ||
                           s_state.state == TOUCH_EVT_UP)
                              ? TOUCH_EVT_DOWN : TOUCH_EVT_MOVE;

            /* 物理速度钳制 */
            {
                int32_t dx = (int32_t)lx - (int32_t)s_state.x;
                int32_t dy = (int32_t)ly - (int32_t)s_state.y;
                if (evt == TOUCH_EVT_MOVE) {
                    if (dx > TOUCH_MAX_STEP) dx = TOUCH_MAX_STEP;
                    if (dx < -TOUCH_MAX_STEP) dx = -TOUCH_MAX_STEP;
                    if (dy > TOUCH_MAX_STEP) dy = TOUCH_MAX_STEP;
                    if (dy < -TOUCH_MAX_STEP) dy = -TOUCH_MAX_STEP;
                    lx = (uint16_t)((int32_t)s_state.x + dx);
                    ly = (uint16_t)((int32_t)s_state.y + dy);
                }
            }

            /* 轻平滑（首次按下不滤波，响应零延迟）：(新 + 7*旧) >> 3 */
            if (evt == TOUCH_EVT_MOVE) {
                lx = (uint16_t)(((uint32_t)lx + 7u * s_state.x) >> 3u);
                ly = (uint16_t)(((uint32_t)ly + 7u * s_state.y) >> 3u);
            }
            if (evt == TOUCH_EVT_DOWN) {
                s_state.x = lx;   /* DOWN 用原始值，不叠加旧状态 */
                s_state.y = ly;
                s_state.down_x = lx;
                s_state.down_y = ly;
                s_state.max_dx = 0;
                s_state.max_dy = 0;
            } else {
                s_state.x = lx;
                s_state.y = ly;
                /* 追踪全程最大位移（接触抖动/中途瞬断不影响手势判定） */
                uint16_t dx = (lx > s_state.down_x)
                                  ? (uint16_t)(lx - s_state.down_x)
                                  : (uint16_t)(s_state.down_x - lx);
                uint16_t dy = (ly > s_state.down_y)
                                  ? (uint16_t)(ly - s_state.down_y)
                                  : (uint16_t)(s_state.down_y - ly);
                if (dx > s_state.max_dx) s_state.max_dx = dx;
                if (dy > s_state.max_dy) s_state.max_dy = dy;
            }
            s_state.raw_x = (uint16_t)rx;
            s_state.raw_y = (uint16_t)ry;
            s_state.state = evt;
            s_state.gen++;
        }
        vTaskDelay(pdMS_TO_TICKS(TSV_SAMPLE_MS));
    }
}

/* ---------- 四角校准 ---------- */
typedef struct {
    uint16_t lx, ly;    /* 逻辑角点 */
    int32_t  rx, ry;    /* 采集的物理 AD */
} cal_corner_t;

static uint16_t s_cal_cx, s_cal_cy;   /* 当前十字位置（渲染任务绘制） */

/* 在渲染任务内绘制：清屏 + 十字 + 提示 */
static void cal_draw_cb(void)
{
    uint16_t w = BSP_LCD_GetWidth();
    uint16_t h = BSP_LCD_GetHeight();
    BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                 BSP_LCD_COLOR_BLACK);
    BSP_LCD_ShowString(8, 8, "TOUCH CAL: tap cross", BSP_LCD_COLOR_WHITE,
                       BSP_LCD_FONT_16);
    BSP_LCD_Fill((uint16_t)(s_cal_cx - 4), (uint16_t)(s_cal_cy - 1),
                 (uint16_t)(s_cal_cx + 4), (uint16_t)(s_cal_cy + 1),
                 BSP_LCD_COLOR_WHITE);
    BSP_LCD_Fill((uint16_t)(s_cal_cx - 1), (uint16_t)(s_cal_cy - 4),
                 (uint16_t)(s_cal_cx + 1), (uint16_t)(s_cal_cy + 4),
                 BSP_LCD_COLOR_WHITE);
}

/* 等待一次按下-抬起，返回最后有效物理坐标；超时返回 0 */
static uint8_t cal_wait_tap(int32_t *rx, int32_t *ry, uint32_t timeout_ms)
{
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint8_t pressed = 0;
    int32_t sx = 0, sy = 0;

    while ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0 < timeout_ms) {
        int32_t a, b;
        if (BSP_Touch_ReadRaw(&a, &b)) {
            pressed = 1;
            sx = a;   /* 按住期间持续刷新 */
            sy = b;
        } else if (pressed) {
            *rx = sx;
            *ry = sy;
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

void TouchSvc_Calibrate(void)
{
    static const uint16_t corners[4][2] = {
        { 20, 20 }, { 220, 20 }, { 220, 300 }, { 20, 300 },
    };
    cal_corner_t pts[4];
    uint8_t ok = 1;

    LOG_Printf("[TOUCH] calibrate: tap the 4 crosses (TL/TR/BR/BL)\r\n");
    for (uint8_t i = 0; i < 4; i++) {
        pts[i].lx = corners[i][0];
        pts[i].ly = corners[i][1];
        s_cal_cx = pts[i].lx;
        s_cal_cy = pts[i].ly;
        cal_draw_cb();   /* LVGL 接管后直接绘制校准十字（BSP 层，短暂覆盖画面可接受） */
        LOG_Printf("[TOUCH] cal: tap corner %u @(%u,%u)...\r\n",
                   (unsigned)(i + 1), (unsigned)pts[i].lx, (unsigned)pts[i].ly);

        if (cal_wait_tap(&pts[i].rx, &pts[i].ry, 20000u)) {
            LOG_Printf("[TOUCH] cal: corner %u raw=(%ld,%ld)\r\n",
                       (unsigned)(i + 1), (long)pts[i].rx, (long)pts[i].ry);
        } else {
            ok = 0;
            LOG_Printf("[TOUCH] cal: corner %u timeout\r\n", (unsigned)(i + 1));
            break;
        }
    }

    if (ok) {
        /* 轴对齐线性映射（支持各轴反向） */
        int32_t sx = (pts[1].rx - pts[0].rx) + (pts[2].rx - pts[3].rx);  /* 左→右 */
        int32_t sy = (pts[3].ry - pts[0].ry) + (pts[2].ry - pts[1].ry);  /* 上→下 */
        bsp_touch_cal_t cal;
        BSP_Touch_GetCal(&cal);
        cal.xfac = sx / 2 / 200;
        cal.yfac = sy / 2 / 280;
        cal.xc = pts[0].rx - cal.xfac * (int32_t)(pts[0].lx - 120);
        cal.yc = pts[0].ry - cal.yfac * (int32_t)(pts[0].ly - 160);
        cal.valid = (cal.xfac != 0 && cal.yfac != 0) ? 1u : 0u;
        BSP_Touch_SetCal(&cal);
        if (cal.valid) {
            if (UsrStore_Set(USR_KEY_TOUCH_CAL, &cal, sizeof(cal)) == 0) {
                LOG_Printf("[TOUCH] cal saved to EEPROM\r\n");
            } else {
                LOG_Printf("[TOUCH] WARN cal save to EEPROM failed\r\n");
            }
        }
        LOG_Printf("[TOUCH] cal: done xfac=%ld yfac=%ld xc=%ld yc=%ld %s\r\n",
                   (long)cal.xfac, (long)cal.yfac, (long)cal.xc, (long)cal.yc,
                   cal.valid ? "OK" : "FAIL");
    } else {
        LOG_Printf("[TOUCH] cal: aborted\r\n");
    }

}

/* ---------- 接口 ---------- */
void TouchSvc_Init(void)
{
    BSP_Touch_Init();
    /* 从 EEPROM 恢复上次校准（若存在） */
    bsp_touch_cal_t cal;
    if (UsrStore_Get(USR_KEY_TOUCH_CAL, &cal, sizeof(cal)) == (int)sizeof(cal) &&
        cal.valid) {
        BSP_Touch_SetCal(&cal);
        LOG_Printf("[TOUCH] cal restored from EEPROM "
                   "(xfac=%ld yfac=%ld xc=%ld yc=%ld)\r\n",
                   (long)cal.xfac, (long)cal.yfac,
                   (long)cal.xc, (long)cal.yc);
    }
    if (s_task != NULL) return;

    osThreadAttr_t attr = {
        .name = "TouchSvc",
        .stack_size = TSV_TASK_STACK,
        .priority = osPriorityAboveNormal,
    };
    s_task = osThreadNew(touch_svc_task, NULL, &attr);
}

const touch_svc_state_t *TouchSvc_GetState(void)
{
    return &s_state;
}
