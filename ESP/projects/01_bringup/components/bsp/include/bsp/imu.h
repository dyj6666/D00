/* ================================================================
 * imu.h —— QMI8658A 六轴姿态传感器（I2C 0x6A）
 *
 * 提供三轴加速度（单位 g）、三轴角速度（单位 dps）、芯片温度（℃）。
 * 中断引脚接 XL9555 的 P0_1。
 *
 * 性能要点：
 *   - 12 字节加速度+角速度**一次事务读完**（厂商例程同样是一次 12 字节）；
 *   - 灵敏度由量程即时算出（32768/满量程），不查表、不做浮点除法以外的工作；
 *   - 采样率/量程可配，默认 ±8g / ±512dps / 500Hz，兼顾分辨率与动态范围。
 *
 * 说明：本驱动默认不开低通滤波器（CTRL5 = 0），读数会更"原始"。
 *       需要 LPF 时可用 bsp_imu_write_reg() 按数据手册 CTRL5 的位定义自行配置。
 * ================================================================ */
#ifndef __BSP_IMU_H
#define __BSP_IMU_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次采样结果 */
typedef struct
{
    bsp_vec3_t acc_g;       /*!< 加速度，单位 g */
    bsp_vec3_t gyro_dps;    /*!< 角速度，单位 °/s */
    float      temp_c;      /*!< 芯片温度，单位 ℃ */
} bsp_imu_data_t;

/** 原始量（未换算，调试/算法自处理用） */
typedef struct
{
    int16_t acc[3];
    int16_t gyro[3];
    int16_t temp;
} bsp_imu_raw_t;

/** 配置：量程与输出速率 */
typedef struct
{
    uint16_t acc_range_g;       /*!< 2 / 4 / 8 / 16 */
    uint16_t acc_odr_hz;        /*!< 31 / 62 / 125 / 250 / 500 / 1000 / 2000 / 4000 / 8000 */
    uint16_t gyro_range_dps;    /*!< 16 / 32 / 64 / 128 / 256 / 512 / 1024 / 2048 */
    uint16_t gyro_odr_hz;       /*!< 同 acc_odr_hz */
} bsp_imu_cfg_t;

/** 默认配置：±8g / ±512dps / 500Hz */
#define BSP_IMU_CFG_DEFAULT     { 8U, 500U, 512U, 500U }

/**
 * @brief  覆盖默认配置（必须在 bsp_imu_init 之前调用）
 * @return ESP_OK；参数不在支持范围返回 ESP_ERR_INVALID_ARG
 */
esp_err_t bsp_imu_configure(const bsp_imu_cfg_t *cfg);

/**
 * @brief  初始化（幂等）：复位 -> 校验 WHO_AM_I -> 配置量程/速率 -> 使能
 */
esp_err_t bsp_imu_init(void);

/**
 * @brief  反初始化：关闭传感器
 */
esp_err_t bsp_imu_deinit(void);

/**
 * @brief  读取换算后的三轴数据 + 温度（一次 I2C 事务读 12 字节加速度/角速度）
 */
esp_err_t bsp_imu_read(bsp_imu_data_t *out);

/**
 * @brief  只读加速度（12 字节事务内部只用前 6 字节，仍一次读完更省总线）
 */
esp_err_t bsp_imu_read_acc(bsp_vec3_t *acc_g);

/**
 * @brief  读原始值（不做单位换算）
 */
esp_err_t bsp_imu_read_raw(bsp_imu_raw_t *out);

/**
 * @brief  读取芯片温度（℃）
 */
esp_err_t bsp_imu_read_temperature(float *temp_c);

/**
 * @brief  中断引脚当前电平（经 XL9555 P0_1，true = 有事件）
 */
bool bsp_imu_irq_asserted(void);

/**
 * @brief  读器件 ID（WHO_AM_I，正常为 0x05）
 */
esp_err_t bsp_imu_read_id(uint8_t *whoami, uint8_t *revision);

/**
 * @brief  直接读写寄存器（高级用法：配置 LPF/中断/FIFO 等）
 */
esp_err_t bsp_imu_write_reg(uint8_t reg, uint8_t val);
esp_err_t bsp_imu_read_reg(uint8_t reg, uint8_t *val, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_IMU_H */
