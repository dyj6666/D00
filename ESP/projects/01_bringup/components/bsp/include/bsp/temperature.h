/* ================================================================
 * temperature.h —— ESP32-S3 片内温度传感器
 *
 * 说明：测的是芯片结温，比环境温度高，通常用于"看芯片是否过热"，
 *       不能当环境温度计用（需要环境温度请接外置传感器）。
 * 精度：原厂标称 ±1℃（0~70℃ 区间），实际约 ±2~3℃。
 * ================================================================ */
#ifndef __BSP_TEMPERATURE_H
#define __BSP_TEMPERATURE_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  安装并启用片内温度传感器（幂等）
 */
esp_err_t bsp_temperature_init(void);

/**
 * @brief  卸载温度传感器
 */
esp_err_t bsp_temperature_deinit(void);

/**
 * @brief  读取当前芯片温度（℃）
 */
esp_err_t bsp_temperature_read(float *celsius);

/**
 * @brief  读多次取平均（更稳，建议 times >= 8）
 */
esp_err_t bsp_temperature_read_avg(uint8_t times, float *celsius);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_TEMPERATURE_H */
