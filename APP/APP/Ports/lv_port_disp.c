/* ================================================================
 * lv_port_disp —— LVGL 显示端口实现（FSMC 并口 TFTLCD 240x320）
 *
 * 架构位置：APP Ports 层；flush 回调直接映射到 BSP_LCD_WritePixels
 *           （FSMC BANK4 16 位并口），不持有任何业务逻辑。
 *
 * 内存与传输策略（性能拉满版，2026-08 实测）：
 *   绘制缓冲：主 SRAM 单缓冲 26 行（12.5KB，读 66MB/s vs 外部 SRAM 18MB/s）
 *     —— 同步 flush 架构下双缓冲无并行收益（flush 同步完成才继续渲染），
 *        单缓冲省内存换更大行深：块数 27→13，对象遍历开销减半。
 *   传输路径：CPU 32bit 双像素写（FSMC 自动拆两次 16bit 总线写）。
 *   异步 DMA flush（DMA2 Stream1/4 M2P 直写 FSMC）已实测废弃：
 *     总线级死锁（DMA 与 CPU 并发访问 FSMC 时互锁，PC 卡死在 EN 轮询/
 *     FSMC 写，且连带 ETH 无响应），详见 ENGINEERING_LOG 10.55。
 *   原外部 SRAM 槽（mem_map.h MEM_LVGL_DISP_*）保留作未来图像缓存。
 * ================================================================ */
#include "lvgl.h"
#include "lv_port_disp.h"
#include "bsp_lcd.h"
#include "mem_map.h"
#include "stm32f4xx.h"   /* DWT 周期计数器 */

#define DISP_HOR_RES   240u
#define DISP_VER_RES   320u
/* 绘制缓冲：主 SRAM 双缓冲 12 行（2×5.8KB，实测稳定版）。
 * 注：单缓冲 26 行（块数 13）与 32bit FSMC 拆写曾致镜像花屏，回退。 */
#define DISP_BUF_ROWS  12u
#define DISP_BUF_SIZE  (DISP_HOR_RES * DISP_BUF_ROWS)

/* 编译期预算断言：双缓冲字节数（RGB565=2B/px）不得超主 SRAM 剩余预算
 * （当前实测剩余 ~13KB）。任何扩大缓冲的改动在此显式暴露。 */
#if (DISP_BUF_SIZE * 2u * 2u) > (13u * 1024u)
#error "LVGL draw buffer exceeds main SRAM budget (13KB); shrink DISP_BUF_ROWS"
#endif

static lv_color_t s_disp_buf1[DISP_BUF_SIZE] __attribute__((aligned(4)));
static lv_color_t s_disp_buf2[DISP_BUF_SIZE] __attribute__((aligned(4)));

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp;

/* ---- flush 性能统计（DWT CYCCNT @168MHz） ---- */
static lv_flush_stats_t s_flush_stats;

void LvPort_FlushStatsReset(void)
{
    s_flush_stats.calls  = 0;
    s_flush_stats.cycles = 0;
    s_flush_stats.pixels = 0;
}

void LvPort_FlushStatsGet(lv_flush_stats_t *out)
{
    if (out != NULL) {
        *out = s_flush_stats;
    }
}

/* ---------------- LVGL flush 回调：区域像素同步直写 LCD ---------------- */
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                       lv_color_t *color_p)
{
    uint32_t t0 = DWT->CYCCNT;
    /* LVGL v8 区域字段为 x1/y1/x2/y2；可能因扫描方向出现反向，
     * 统一取 min/max 归一化 */
    uint16_t x0 = (uint16_t)(area->x1 < area->x2 ? area->x1 : area->x2);
    uint16_t x1 = (uint16_t)(area->x1 < area->x2 ? area->x2 : area->x1);
    uint16_t y0 = (uint16_t)(area->y1 < area->y2 ? area->y1 : area->y2);
    uint16_t y1 = (uint16_t)(area->y1 < area->y2 ? area->y2 : area->y1);

    BSP_LCD_WritePixels(x0, y0, (uint16_t)(x1 - x0 + 1u),
                        (uint16_t)(y1 - y0 + 1u),
                        (const uint16_t *)color_p);

    s_flush_stats.calls++;
    s_flush_stats.cycles += (DWT->CYCCNT - t0);
    s_flush_stats.pixels += (uint64_t)(x1 - x0 + 1u) * (y1 - y0 + 1u);

    lv_disp_flush_ready(drv);   /* 同步写完成，立即就绪 */
}

/* ---------------- 显示端口初始化 ---------------- */
void LvPort_DispInit(void)
{
    /* LCD 硬件已由 LcdApp 模块初始化（BSP_LCD_Init），此处仅注册 LVGL 显示 */
    lv_disp_draw_buf_init(&s_draw_buf, s_disp_buf1, s_disp_buf2, DISP_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf  = &s_draw_buf;   /* 关键：显式绑定绘制缓冲，否则 LVGL 用未初始化内部缓冲 */
    s_disp_drv.hor_res   = DISP_HOR_RES;
    s_disp_drv.ver_res   = DISP_VER_RES;
    s_disp_drv.flush_cb  = disp_flush;
    s_disp = lv_disp_drv_register(&s_disp_drv);
    (void)s_disp;   /* 首个 display 自动成为默认，句柄保留供后续多屏扩展 */
}
