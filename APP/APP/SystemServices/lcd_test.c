/* ================================================================
 * lcd_test —— LCD 测试：基准/色块/刷新率验证
 *
 * 架构位置：APP 服务层；产线与调试自检
 * ================================================================ */
#include "lcd_test.h"
#include "lcd_ui.h"
#include "bsp_lcd.h"
#include "LCD/lcd.h"
#include "FreeRTOS.h"
#include "task.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>

/* 字体点阵由 BSP/LCD/lcd.c 定义（lcdfont.h 内为全局数组，不可重复包含） */
extern const unsigned char asc2_1608[95][16];

/* ================================================================
 * LCD 自动化测试实现
 * 读回原理：FSMC 读时序 DATAST=60（360ns，慢而可靠），
 * 写-读回像素级比对可捕获写坏 GRAM / 窗口错位 / 文字偏移等缺陷。
 * ================================================================ */

#define TEST_GRID_X     6   /* 全屏采样网格 6x8 = 48 点 */
#define TEST_GRID_Y     8
#define TEST_CAL_N      5   /* 校准色数：黑/红/绿/蓝/白 */

typedef struct {
    uint16_t color;     /* 写入值 */
    uint16_t read;      /* 读回值（校准映射） */
} lcd_test_cal_t;

typedef struct {
    uint16_t checks;
    uint16_t fails;
    uint16_t first_x, first_y;
    uint16_t first_got, first_exp;
} lcd_test_stat_t;

static const uint16_t s_cal_colors[TEST_CAL_N] = {
    BSP_LCD_COLOR_BLACK, BSP_LCD_COLOR_RED, BSP_LCD_COLOR_GREEN,
    BSP_LCD_COLOR_BLUE,  BSP_LCD_COLOR_WHITE,
};

static lcd_test_cal_t s_cal[TEST_CAL_N];

/* ---------- 校验辅助 ---------- */
static void test_check(lcd_test_stat_t *st, uint16_t x, uint16_t y,
                       uint16_t got, uint16_t exp)
{
    st->checks++;
    if (got != exp) {
        if (st->fails == 0) {
            st->first_x = x;
            st->first_y = y;
            st->first_got = got;
            st->first_exp = exp;
        }
        st->fails++;
    }
}

static void test_report(const char *name, const lcd_test_stat_t *st,
                        uint32_t ms)
{
    if (st->fails == 0) {
        LOG_Printf("[LCD] %-12s: PASS  (%u checks, %lu ms)\r\n",
                   name, (unsigned)st->checks, (unsigned long)ms);
    } else {
        LOG_Printf("[LCD] %-12s: FAIL  (%u/%u, first@(%u,%u) got=0x%04X "
                   "exp=0x%04X, %lu ms)\r\n",
                   name, (unsigned)st->fails, (unsigned)st->checks,
                   (unsigned)st->first_x, (unsigned)st->first_y,
                   (unsigned)st->first_got, (unsigned)st->first_exp,
                   (unsigned long)ms);
    }
}

static uint16_t test_read(uint16_t x, uint16_t y)
{
    return lcd_read_point_rgb565(x, y);
}

/* ---------- 校准：建立 写入色 → 读回色 映射 ---------- */
static void test_calibrate(lcd_test_stat_t *st)
{
    for (int i = 0; i < TEST_CAL_N; i++) {
        BSP_LCD_Fill(0, 0, 15, 15, s_cal_colors[i]);
        s_cal[i].color = s_cal_colors[i];
        s_cal[i].read = test_read(7, 7);
        LOG_Printf("[LCD] cal %-5s write=0x%04X read=0x%04X\r\n",
                   i == 0 ? "BLACK" : i == 1 ? "RED" :
                   i == 2 ? "GREEN" : i == 3 ? "BLUE" : "WHITE",
                   (unsigned)s_cal[i].color, (unsigned)s_cal[i].read);
    }
    (void)st;
}

/* 按校准色匹配：写色 → 读回期望值 */
static uint16_t test_exp(uint16_t write_color)
{
    for (int i = 0; i < TEST_CAL_N; i++) {
        if (s_cal[i].color == write_color) return s_cal[i].read;
    }
    return write_color;
}

/* ================= SelfTest ================= */
void LcdTest_RunSelfTest(void)
{
    uint16_t w = BSP_LCD_GetWidth(), h = BSP_LCD_GetHeight();
    lcd_test_stat_t st = {0};
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    test_calibrate(&st);

    /* 1. 全屏填充读回：每种校准色全屏填充 + 网格采样 */
    for (int c = 0; c < TEST_CAL_N; c++) {
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     s_cal[c].color);
        for (int gy = 0; gy < TEST_GRID_Y; gy++) {
            for (int gx = 0; gx < TEST_GRID_X; gx++) {
                uint16_t x = (uint16_t)((gx + 1) * (w / (TEST_GRID_X + 1)));
                uint16_t y = (uint16_t)((gy + 1) * (h / (TEST_GRID_Y + 1)));
                test_check(&st, x, y, test_read(x, y), s_cal[c].read);
            }
        }
    }
    test_report("fill", &st, 0); /* 时间合并到最后报告 */

    /* 2. 窗口边界：红底上画 50x50 蓝窗，窗内/窗外校验 */
    {
        lcd_test_stat_t ws = {0};
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     BSP_LCD_COLOR_RED);
        BSP_LCD_Fill(50, 50, 99, 99, BSP_LCD_COLOR_BLUE);
        /* 窗内 */
        test_check(&ws, 50, 50, test_read(50, 50), test_exp(BSP_LCD_COLOR_BLUE));
        test_check(&ws, 99, 99, test_read(99, 99), test_exp(BSP_LCD_COLOR_BLUE));
        test_check(&ws, 74, 74, test_read(74, 74), test_exp(BSP_LCD_COLOR_BLUE));
        /* 窗外四角 */
        test_check(&ws, 49, 49, test_read(49, 49), test_exp(BSP_LCD_COLOR_RED));
        test_check(&ws, 100, 49, test_read(100, 49), test_exp(BSP_LCD_COLOR_RED));
        test_check(&ws, 49, 100, test_read(49, 100), test_exp(BSP_LCD_COLOR_RED));
        test_check(&ws, 100, 100, test_read(100, 100), test_exp(BSP_LCD_COLOR_RED));
        test_report("window", &ws, 0);
        st.checks += ws.checks; st.fails += ws.fails;
    }

    /* 3. 单点窗口：蓝底上画 1x1 红点，邻点不受影响 */
    {
        lcd_test_stat_t ps = {0};
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     BSP_LCD_COLOR_BLUE);
        BSP_LCD_Fill(120, 120, 120, 120, BSP_LCD_COLOR_RED);
        test_check(&ps, 120, 120, test_read(120, 120),
                   test_exp(BSP_LCD_COLOR_RED));
        test_check(&ps, 119, 120, test_read(119, 120),
                   test_exp(BSP_LCD_COLOR_BLUE));
        test_check(&ps, 121, 120, test_read(121, 120),
                   test_exp(BSP_LCD_COLOR_BLUE));
        test_check(&ps, 120, 119, test_read(120, 119),
                   test_exp(BSP_LCD_COLOR_BLUE));
        test_check(&ps, 120, 121, test_read(120, 121),
                   test_exp(BSP_LCD_COLOR_BLUE));
        test_report("1px-window", &ps, 0);
        st.checks += ps.checks; st.fails += ps.fails;
    }

    /* 4. 像素级字符渲染：'A' @ (20,30)，128 像素逐一比对字体点阵 */
    {
        lcd_test_stat_t cs = {0};
        const uint8_t *font = asc2_1608['A' - ' '];
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     BSP_LCD_COLOR_BLACK);
        BSP_LCD_ShowChar(20, 30, 'A', BSP_LCD_COLOR_WHITE, BSP_LCD_FONT_16);
        for (uint16_t row = 0; row < 16; row++) {
            uint8_t bi = (uint8_t)(row >> 3);
            uint8_t bit = (uint8_t)(7u - (row & 7u));
            for (uint16_t col = 0; col < 8; col++) {
                uint16_t exp = (font[col * 2 + bi] & (1u << bit))
                                   ? test_exp(BSP_LCD_COLOR_WHITE)
                                   : test_exp(BSP_LCD_COLOR_BLACK);
                test_check(&cs, (uint16_t)(20 + col), (uint16_t)(30 + row),
                           test_read((uint16_t)(20 + col),
                                     (uint16_t)(30 + row)), exp);
            }
        }
        /* 字符窗口外 1px 不得串色 */
        test_check(&cs, 19, 38, test_read(19, 38),
                   test_exp(BSP_LCD_COLOR_BLACK));
        test_check(&cs, 28, 38, test_read(28, 38),
                   test_exp(BSP_LCD_COLOR_BLACK));
        test_report("char", &cs, 0);
        st.checks += cs.checks; st.fails += cs.fails;
    }

    /* 5. 字符串渲染：'AB' @ (20,60)，2 字形 + 1px 间距全窗口比对 */
    {
        lcd_test_stat_t ss = {0};
        const uint8_t *fa = asc2_1608['A' - ' '];
        const uint8_t *fb = asc2_1608['B' - ' '];
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     BSP_LCD_COLOR_BLACK);
        BSP_LCD_ShowString(20, 60, "AB", BSP_LCD_COLOR_WHITE,
                           BSP_LCD_FONT_16);
        for (uint16_t row = 0; row < 16; row++) {
            uint8_t bi = (uint8_t)(row >> 3);
            uint8_t bit = (uint8_t)(7u - (row & 7u));
            for (uint16_t col = 0; col < 17; col++) {   /* 8+1+8 */
                uint16_t exp;
                if (col < 8) {
                    exp = (fa[col * 2 + bi] & (1u << bit))
                              ? test_exp(BSP_LCD_COLOR_WHITE)
                              : test_exp(BSP_LCD_COLOR_BLACK);
                } else if (col == 8) {
                    exp = test_exp(BSP_LCD_COLOR_BLACK);   /* 1px 间距 */
                } else {
                    exp = (fb[(col - 9) * 2 + bi] & (1u << bit))
                              ? test_exp(BSP_LCD_COLOR_WHITE)
                              : test_exp(BSP_LCD_COLOR_BLACK);
                }
                test_check(&ss, (uint16_t)(20 + col), (uint16_t)(60 + row),
                           test_read((uint16_t)(20 + col),
                                     (uint16_t)(60 + row)), exp);
            }
        }
        test_report("string", &ss, 0);
        st.checks += ss.checks; st.fails += ss.fails;
    }

    uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0;
    test_report("SELFTEST", &st, ms);
}

/* ================= Soak ================= */
void LcdTest_RunSoak(uint16_t seconds)
{
    uint16_t w = BSP_LCD_GetWidth(), h = BSP_LCD_GetHeight();
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t end = t0 + (uint32_t)seconds * 1000u;
    uint32_t heap0 = xPortGetFreeHeapSize();
    uint32_t cycles = 0;
    uint32_t errs = 0;

    /* 先校准（同 SelfTest 的映射） */
    for (int i = 0; i < TEST_CAL_N; i++) {
        BSP_LCD_Fill(0, 0, 15, 15, s_cal_colors[i]);
        s_cal[i].color = s_cal_colors[i];
        s_cal[i].read = test_read(7, 7);
    }

    LOG_Printf("[LCD] soak start: %u s, heap=%lu B\r\n",
               (unsigned)seconds, (unsigned long)heap0);
    while ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) < end) {
        /* 混合负载：清屏 + 色块 + 字符串 + 数字 */
        BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1),
                     BSP_LCD_COLOR_BLACK);
        BSP_LCD_Fill(100, 100, 115, 115, BSP_LCD_COLOR_BLUE);
        BSP_LCD_ShowString(8, 8, "D00 LCD SOAK TEST", BSP_LCD_COLOR_WHITE,
                           BSP_LCD_FONT_16);
        BSP_LCD_ShowString(8, 32, "abcdefghijklmnopqrstuvwxyz0123456789",
                           BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_12);
        BSP_LCD_ShowNum(8, 52, (uint32_t)cycles, 8, BSP_LCD_COLOR_CYAN,
                        BSP_LCD_FONT_16);

        /* 每轮读回校验：蓝块中心 + 左上角黑底 */
        if (test_read(108, 108) != test_exp(BSP_LCD_COLOR_BLUE)) errs++;
        if (test_read(0, 0) != test_exp(BSP_LCD_COLOR_BLACK)) errs++;

        cycles++;
        vTaskDelay(pdMS_TO_TICKS(2));   /* 让出调度，保持系统可响应 */
    }

    uint32_t heap1 = xPortGetFreeHeapSize();
    uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0;
    LOG_Printf("[LCD] soak done: cycles=%lu errs=%lu heap %lu -> %lu B "
               "(%+ld), %lu ms\r\n",
               (unsigned long)cycles, (unsigned long)errs,
               (unsigned long)heap0, (unsigned long)heap1,
               (long)((int32_t)heap1 - (int32_t)heap0),
               (unsigned long)ms);
}

/* ================= Stress ================= */
void LcdTest_RunStress(uint16_t count)
{
    uint32_t heap0 = xPortGetFreeHeapSize();
    uint32_t drops = 0;
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    for (uint16_t i = 0; i < count; i++) {
        if (LcdUI_NextPage() != 0) drops++;
        if ((i & 0x1F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* 等待队列排空（UI 任务逐条执行，每条含 40ms 防撕裂延时） */
    uint32_t wait0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    while (LcdUI_GetPendingCount() > 0 &&
           (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - wait0 < 20000u) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint32_t heap1 = xPortGetFreeHeapSize();
    uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0;
    uint16_t pending = LcdUI_GetPendingCount();
    LOG_Printf("[LCD] stress done: queued=%u drops=%lu pending=%u page=%u "
               "heap %lu -> %lu B (%+ld), %lu ms\r\n",
               (unsigned)count, (unsigned long)drops, (unsigned)pending,
               (unsigned)LcdUI_GetPage(),
               (unsigned long)heap0, (unsigned long)heap1,
               (long)((int32_t)heap1 - (int32_t)heap0),
               (unsigned long)ms);
}
