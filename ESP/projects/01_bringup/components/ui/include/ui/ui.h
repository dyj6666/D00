/* ================================================================
 * ui.h —— LVGL 界面入口
 *
 * 典型用法（app_main 里，bsp_init 之后）：
 *     ui_start();          // 初始化 LVGL + 建界面 + 起刷新任务（非阻塞）
 *
 * 设计要点（性能关键）：
 *   - 显示：双缓冲（内部 RAM）+ 异步 DMA 队列重叠，见 ui_port.c；
 *   - 输入：触摸 20ms 轮询（一次 I2C 事务 ~0.65ms），按键走 BSP 事件；
 *   - 任务：LVGL 独占 core1，应用逻辑留在 core0，互不抢 CPU。
 * ================================================================ */
#ifndef __UI_H
#define __UI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  启动 UI：初始化 LVGL 端口、构建界面、创建刷新任务（幂等）
 * @return ESP_OK 成功；其它为失败原因（此时屏幕保持 BSP 原状）
 */
esp_err_t ui_start(void);

/**
 * @brief  停止 UI 刷新任务（界面冻结，LVGL 不释放；调试用）
 */
esp_err_t ui_stop(void);

/**
 * @brief  实测显示帧率（每秒完成上屏的次数）
 */
uint32_t ui_get_fps(void);

/**
 * @brief  当前页面索引：0=概览 1=姿态 2=系统
 */
uint8_t ui_get_page_index(void);

/**
 * @brief  当前屏幕截图（RGB565 原始流）经控制台串口回传，供主机转 PNG 做视觉回归
 * @note   输出：文本头 "DUMP <w> <h>" + 换行 + w*h*2 字节 RGB565(小端) + 回车换行 + "DUMP_END"
 */
void ui_debug_dump_screen(void);

/**
 * @brief  切到下一页（按键 K1 调用）
 */
void ui_next_page(void);

/**
 * @brief  切到上一页（按键 K2 调用）
 */
void ui_prev_page(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_H */
