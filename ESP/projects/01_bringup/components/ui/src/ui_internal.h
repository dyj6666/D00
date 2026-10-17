/* ================================================================
 * ui_internal.h —— ui 组件内部接口（不对外导出）
 * ================================================================ */
#ifndef __UI_INTERNAL_H
#define __UI_INTERNAL_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 构建全部页面与数据定时器（只能在 LVGL 任务/初始化阶段调用） */
void ui_dashboard_build(void);

/** 相对切页：dir > 0 下一页，dir < 0 上一页（只能在 LVGL 任务里调用） */
void ui_dashboard_switch(int8_t dir);

/**
 * @brief  请求切页（任意任务/ISR 安全）
 * @note   只置一个待处理标志，真正的 lv_scr_load_anim 由 LVGL 任务里的
 *         导航定时器执行——LVGL 不是线程安全的，跨任务调用会踩坏内部状态。
 */
void ui_dashboard_request(int8_t dir);

/** 当前页索引 */
uint8_t ui_dashboard_page(void);

/** 绝对跳页（页签/热区用） */
void ui_dashboard_goto(uint8_t page);

/** 瞬时跳页（无动画） */
void ui_dashboard_show(uint8_t page);

#ifdef __cplusplus
}
#endif

#endif /* __UI_INTERNAL_H */
