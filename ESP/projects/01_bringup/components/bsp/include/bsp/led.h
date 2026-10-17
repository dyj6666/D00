/* ================================================================
 * led.h —— 板载 LED 驱动
 *
 * 硬件：
 *   蓝色 LED（LEDB） -> IO4，**低电平点亮**，用 LEDC 硬件 PWM 调亮度；
 *   红色 LED（LEDR） -> XL9555 P1_0，**低电平点亮**，只能开/关；
 *   电源指示灯（PWR）不受软件控制。
 *
 * 性能设计：
 *   - 调亮度/呼吸全部交给 LEDC 硬件渐变（ledc_set_fade_with_time），
 *     CPU 只发一条指令，渐变过程零占用；
 *   - 闪烁用 esp_timer 周期回调（周期级唤醒，不是忙等）；
 *   - 蓝灯的"呼吸"= 每半周期翻转一次硬件渐变目标，视觉上是连续正弦感。
 * ================================================================ */
#ifndef __BSP_LED_H
#define __BSP_LED_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LED 逻辑编号 */
typedef enum
{
    BSP_LED_BLUE = 0,       /*!< IO4，支持 PWM 调光与呼吸 */
    BSP_LED_RED,            /*!< XL9555 P1_0，仅开/关 */
    BSP_LED_COUNT
} bsp_led_t;

/**
 * @brief  初始化 LED（幂等）：配置 LEDC 通道与 IO 扩展上的红灯
 */
esp_err_t bsp_led_init(void);

/**
 * @brief  反初始化：关灯、停止闪烁/呼吸、释放 LEDC 通道
 */
esp_err_t bsp_led_deinit(void);

/**
 * @brief  开关某个 LED（蓝灯等价于亮度 100/0）
 */
esp_err_t bsp_led_set(bsp_led_t led, bool on);

/**
 * @brief  翻转某个 LED
 */
esp_err_t bsp_led_toggle(bsp_led_t led);

/**
 * @brief  查询 LED 当前逻辑状态
 */
bool bsp_led_is_on(bsp_led_t led);

/**
 * @brief  设置蓝色 LED 亮度
 * @param  percent 0~100
 */
esp_err_t bsp_led_set_brightness(uint8_t percent);

/**
 * @brief  获取蓝色 LED 亮度
 */
uint8_t bsp_led_get_brightness(void);

/**
 * @brief  蓝色 LED 硬件渐变到指定亮度
 * @param  percent 目标亮度 0~100
 * @param  ms      渐变时长（毫秒）
 * @note   立即返回，渐变由 LEDC 硬件完成，CPU 零占用。
 */
esp_err_t bsp_led_fade_to(uint8_t percent, uint32_t ms);

/**
 * @brief  启动蓝色 LED 呼吸效果
 * @param  half_period_ms 半周期时长（亮->灭 或 灭->亮 的时长）
 */
esp_err_t bsp_led_breath_start(uint32_t half_period_ms);

/**
 * @brief  停止呼吸效果（停在当前亮度）
 */
esp_err_t bsp_led_breath_stop(void);

/**
 * @brief  启动闪烁（周期 = 亮 + 灭 的总时长）
 */
esp_err_t bsp_led_blink_start(bsp_led_t led, uint32_t period_ms);

/**
 * @brief  停止闪烁并熄灭该 LED
 */
esp_err_t bsp_led_blink_stop(bsp_led_t led);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_LED_H */
