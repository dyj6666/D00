/* ================================================================
 * beep.h —— 板载有源蜂鸣器
 *
 * 硬件：BEEP 接在 XL9555 的 P0_2 上（经 Q2 扩流），**高电平 = 关**。
 *       有源蜂鸣器自带振荡电路，软件只能控制响/停，不能定音高。
 * ================================================================ */
#ifndef __BSP_BEEP_H
#define __BSP_BEEP_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化蜂鸣器（幂等，确保初始为静音）
 */
esp_err_t bsp_beep_init(void);

/**
 * @brief  反初始化：静音并释放定时器
 */
esp_err_t bsp_beep_deinit(void);

/**
 * @brief  开/关蜂鸣器
 */
esp_err_t bsp_beep_set(bool on);

/**
 * @brief  查询当前状态
 */
bool bsp_beep_is_on(void);

/**
 * @brief  响一声
 * @param  ms 持续时间（毫秒）
 * @note   非阻塞：用一次性定时器自动关闭。
 */
esp_err_t bsp_beep_once(uint32_t ms);

/**
 * @brief  响若干声
 * @param  times   次数（1~10）
 * @param  on_ms   每声时长
 * @param  off_ms  间隔
 * @note   非阻塞：由周期性定时器驱动，结束后自动静音。
 */
esp_err_t bsp_beep_beep(uint8_t times, uint32_t on_ms, uint32_t off_ms);

/**
 * @brief  立即停止（含取消进行中的响铃序列）
 */
esp_err_t bsp_beep_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_BEEP_H */
