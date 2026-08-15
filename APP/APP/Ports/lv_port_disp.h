/* ================================================================
 * lv_port_disp —— LVGL 显示端口接口（flush 统计 + 初始化）
 * ================================================================ */
#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include <stdint.h>

void LvPort_DispInit(void);

/* ---- flush 性能统计（DWT 周期，供 gui bench 分解渲染/传输耗时） ---- */
typedef struct {
    uint32_t calls;        /* flush 调用次数 */
    uint64_t cycles;       /* 累计传输周期（含窗口设置） */
    uint64_t pixels;       /* 累计像素数 */
} lv_flush_stats_t;

void LvPort_FlushStatsReset(void);
void LvPort_FlushStatsGet(lv_flush_stats_t *out);

#endif /* LV_PORT_DISP_H */
