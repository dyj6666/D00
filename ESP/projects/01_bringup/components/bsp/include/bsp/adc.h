/* ================================================================
 * adc.h —— 扩展排针上的 ADC 输入
 *
 * 板上没有专用模拟输入接口，能用的只有 J3/J4 引出的独立 IO：
 *   IO5  -> ADC1_CH4
 *   IO6  -> ADC1_CH5
 *   IO8  -> ADC1_CH7
 *   （IO18 属 ADC2，ADC2 与 WiFi 抢占，故不提供）
 *
 * 特性：
 *   - 12bit 单次转换 + 曲线拟合校准，直接给出 mV；
 *   - 每个通道只配置一次，之后读一次约几十微秒。
 * ================================================================ */
#ifndef __BSP_ADC_H
#define __BSP_ADC_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 可用 ADC 通道（对应扩展排针） */
typedef enum
{
    BSP_ADC_CH_0 = 0,       /*!< IO5 */
    BSP_ADC_CH_1,           /*!< IO6 */
    BSP_ADC_CH_2,           /*!< IO8 */
    BSP_ADC_CH_COUNT
} bsp_adc_ch_t;

/**
 * @brief  初始化 ADC1（幂等）并配置可用通道（12bit，约 0~3.1V 量程）
 */
esp_err_t bsp_adc_init(void);

/**
 * @brief  反初始化
 */
esp_err_t bsp_adc_deinit(void);

/**
 * @brief  读原始值（0~4095）
 */
esp_err_t bsp_adc_read_raw(bsp_adc_ch_t ch, int *raw);

/**
 * @brief  读校准后的电压（mV）
 */
esp_err_t bsp_adc_read_mv(bsp_adc_ch_t ch, int *mv);

/**
 * @brief  读多次取平均（降噪，适合测直流电平）
 * @param  times 采样次数（1~64）
 */
esp_err_t bsp_adc_read_mv_avg(bsp_adc_ch_t ch, uint8_t times, int *mv);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_ADC_H */
