/* ================================================================
 * lv_port_disp —— LVGL 显示端口实现（FSMC 并口 TFTLCD 240x320）
 *
 * 架构位置：APP Ports 层；flush 回调直接映射到 BSP_LCD_WritePixels
 *           （FSMC BANK4 16 位并口），不持有任何业务逻辑。
 *
 * 内存策略（方案B 后外部 SRAM 富余）：
 *   LVGL 堆    0x68080000  128KB（lv_conf.h LV_MEM_ADR）
 *   绘制双缓冲 0x680A0000  2×19.2KB（本文件）
 *   其余 0x680A9A00+ 留给后续图像缓存/资源
 * ================================================================ */
#include "lvgl.h"
#include "lv_port_disp.h"
#include "bsp_lcd.h"

#define DISP_HOR_RES   240u
#define DISP_VER_RES   320u
#define DISP_BUF_ROWS  40u                  /* 1/8 屏高：240×40×2B = 19.2KB/块 */
#define DISP_BUF_SIZE  (DISP_HOR_RES * DISP_BUF_ROWS)

/* 双缓冲固定映射到外部 SRAM（FSMC NE3 @0x68000000，1MB；F407 内部仅 128KB） */
#define DISP_BUF_ADDR1 0x680A0000u
#define DISP_BUF_ADDR2 (DISP_BUF_ADDR1 + DISP_BUF_SIZE * (uint32_t)sizeof(lv_color_t))

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp;

/* ---------------- LVGL flush 回调：区域像素直写 LCD ---------------- */
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                       lv_color_t *color_p)
{
    /* LVGL v8 区域字段为 x1/y1/x2/y2；可能因扫描方向出现反向，
     * 统一取 min/max 归一化 */
    uint16_t x0 = (uint16_t)(area->x1 < area->x2 ? area->x1 : area->x2);
    uint16_t x1 = (uint16_t)(area->x1 < area->x2 ? area->x2 : area->x1);
    uint16_t y0 = (uint16_t)(area->y1 < area->y2 ? area->y1 : area->y2);
    uint16_t y1 = (uint16_t)(area->y1 < area->y2 ? area->y2 : area->y1);

    BSP_LCD_WritePixels(x0, y0, (uint16_t)(x1 - x0 + 1u),
                        (uint16_t)(y1 - y0 + 1u),
                        (const uint16_t *)color_p);

    lv_disp_flush_ready(drv);   /* 同步写完成，立即就绪 */
}

/* ---------------- 显示端口初始化 ---------------- */
void LvPort_DispInit(void)
{
    /* LCD 硬件已由 LcdApp 模块初始化（BSP_LCD_Init），此处仅注册 LVGL 显示 */
    lv_disp_draw_buf_init(&s_draw_buf, (void *)DISP_BUF_ADDR1,
                          (void *)DISP_BUF_ADDR2, DISP_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf  = &s_draw_buf;   /* 关键：显式绑定绘制缓冲，否则 LVGL 用未初始化内部缓冲 */
    s_disp_drv.hor_res   = DISP_HOR_RES;
    s_disp_drv.ver_res   = DISP_VER_RES;
    s_disp_drv.flush_cb  = disp_flush;
    s_disp = lv_disp_drv_register(&s_disp_drv);
    (void)s_disp;   /* 首个 display 自动成为默认，句柄保留供后续多屏扩展 */
}
