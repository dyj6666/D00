/* ================================================================
 * touch.h —— 2.4 寸电容触摸屏（CHSC5432，I2C 0x2E）
 *
 * 通信特性（与普通 I2C 器件不同，容易踩坑）：
 *   - 寄存器地址是 **32 位**，按大端 4 字节发送（0x20 00 00 2C …）；
 *   - 一次读 28 字节即可拿到全部 5 个触摸点，官方建议就是这么读的；
 *   - 复位引脚在 XL9555（P0_6），中断在 XL9555（P1_1）。
 *
 * 坐标方向：
 *   板载屏在 BSP 里按"横屏 320x240"使用（见 lcd.c 的 swap_xy + mirror），
 *   因此这里默认按横屏换算。若你的触摸左右/上下颠倒，
 *   把 BSP_TOUCH_LANDSCAPE 改成 0 或调整下面的镜像开关即可
 *   （不同批次屏模组的贴合方向可能不同，用 bsp_touch_read_raw() 可直接看原始值）。
 * ================================================================ */
#ifndef __BSP_TOUCH_H
#define __BSP_TOUCH_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_TOUCH_MAX_POINTS    5U      /*!< CHSC5432 一次上报最多 5 点 */

/** 是否按横屏换算坐标（1 = 横屏，0 = 竖屏） */
#ifndef BSP_TOUCH_LANDSCAPE
#define BSP_TOUCH_LANDSCAPE     1
#endif

/** 单个触摸点（屏幕坐标） */
typedef struct
{
    uint16_t x;
    uint16_t y;
} bsp_touch_point_t;

/** 一次触摸读数 */
typedef struct
{
    bool               pressed;                     /*!< 是否有触摸 */
    uint8_t            count;                       /*!< 有效点数 0~5 */
    bsp_touch_point_t  point[BSP_TOUCH_MAX_POINTS]; /*!< 有效点在前 */
} bsp_touch_state_t;

/**
 * @brief  初始化（幂等）：硬复位触摸 IC -> 读 PID 校验
 */
esp_err_t bsp_touch_init(void);

/**
 * @brief  反初始化：触摸保持复位
 */
esp_err_t bsp_touch_deinit(void);

/**
 * @brief  读取一次触摸状态（已换算为屏幕坐标）
 * @note   一次 I2C 事务读 28 字节，约 0.5ms
 */
esp_err_t bsp_touch_read(bsp_touch_state_t *out);

/**
 * @brief  读取原始触摸数据（不做横竖屏换算，用于校准/排查方向）
 */
esp_err_t bsp_touch_read_raw(bsp_touch_state_t *out);

/**
 * @brief  读触摸 IC 的 PID（4 字节，低 3 字节有效）
 */
esp_err_t bsp_touch_read_pid(uint32_t *pid);

/**
 * @brief  触摸中断引脚当前电平（经 XL9555 P1_1，true = 有事件）
 */
bool bsp_touch_irq_asserted(void);

/**
 * @brief  硬件复位触摸 IC（会保持约 100ms 复位再释放）
 */
esp_err_t bsp_touch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_TOUCH_H */
