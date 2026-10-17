/* ================================================================
 * lightsensor.h —— AP3216C 光环境三合一传感器（I2C 0x1E）
 *
 * 提供：环境光 ALS、接近 PS、红外 IR。中断引脚接 XL9555 的 P0_0。
 *
 * 性能要点：
 *   - 6 个数据寄存器（0x0A~0x0F）**一次 I2C 事务读完**，
 *     厂商例程是逐字节读 6 次并每字节 delay 10ms（>60ms/次），
 *     这里一次事务约 0.2ms，快 300 倍；
 *   - 只关心环境光时可只用 bsp_light_read_als()（2 字节事务，更快）。
 *
 * 时序约束（数据手册）：ALS/PS/IR 全开时，两次完整采样之间
 * 需要间隔 > 112.5ms，否则数据来不及刷新。只读 ALS 时无此限制。
 * ================================================================ */
#ifndef __BSP_LIGHTSENSOR_H
#define __BSP_LIGHTSENSOR_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次采样得到的原始值（未做物理量换算） */
typedef struct
{
    uint16_t als;           /*!< 环境光原始值（0 表示无效） */
    uint16_t ps;            /*!< 接近原始值（0 表示无效） */
    uint16_t ir;            /*!< 红外原始值（0 表示无效） */
} bsp_light_data_t;

/**
 * @brief  初始化（幂等）：复位 -> 开启 ALS/PS/IR -> 回读校验
 */
esp_err_t bsp_light_init(void);

/**
 * @brief  反初始化：关闭传感器（进入掉电）
 */
esp_err_t bsp_light_deinit(void);

/**
 * @brief  读三个原始值（一次事务）
 * @note   全开模式下建议调用间隔 > 112.5ms
 */
esp_err_t bsp_light_read(bsp_light_data_t *out);

/**
 * @brief  只读环境光（2 字节事务，最快）
 */
esp_err_t bsp_light_read_als(uint16_t *als);

/**
 * @brief  读取中断引脚当前电平（经 XL9555 P0_0，true = 有事件）
 */
bool bsp_light_irq_asserted(void);

/**
 * @brief  写 AP3216C 的配置寄存器（高级用法：ALS 增益/量程等）
 * @param  reg 寄存器地址（如 0x01 ALS 配置、0x02 PS 配置）
 * @param  val 寄存器值（位定义见数据手册）
 * @note   位定义随量程/增益变化，具体取值请查阅《AP3216C 数据手册》。
 */
esp_err_t bsp_light_write_reg(uint8_t reg, uint8_t val);

/**
 * @brief  读 AP3216C 寄存器（高级用法）
 */
esp_err_t bsp_light_read_reg(uint8_t reg, uint8_t *val);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_LIGHTSENSOR_H */
