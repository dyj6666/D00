#ifndef BSP_MPU6050_H
#define BSP_MPU6050_H

#include <stdint.h>

/* ================================================================
 * MPU6050 六轴 IMU BSP（I2C1 @400kHz，PB6=SCL / PB7=SDA）
 *   - WHO_AM_I 校验（0x68/0x69 自动探测）
 *   - 量程 ±250dps / ±2g（最高分辨率），采样率 200Hz，DLPF 98Hz
 *   - 14 字节突发读（加速度+温度+陀螺仪）
 *   - 陀螺零偏 / 加速度偏移校准（需静止、尽量水平）
 *   - I2C 互斥锁 + 总线错误自恢复（工业级健壮性）
 * ================================================================ */

typedef struct {
    int16_t ax, ay, az;   /* 原始加速度 LSB */
    int16_t gx, gy, gz;   /* 原始陀螺 LSB */
    int16_t temp;         /* 原始温度 LSB */
} mpu6050_raw_t;

typedef struct {
    float gx_bias, gy_bias, gz_bias;   /* 陀螺零偏（dps） */
    float ax_off, ay_off, az_off;      /* 加速度偏移（g，水平校准时=0附近） */
    uint8_t valid;
} mpu6050_cal_t;

/* 量程换算常数 */
#define MPU6050_ACCEL_SCALE   (16384.0f)   /* ±2g */
#define MPU6050_GYRO_SCALE    (131.0f)     /* ±250dps */
#define MPU6050_TEMP_SCALE    (340.0f)
#define MPU6050_TEMP_OFFSET   (36.53f)

/* 初始化（自动探测地址 + 配置），返回 0=成功 */
uint8_t BSP_MPU6050_Init(void);

/* WHO_AM_I 校验，返回 0=正常 */
uint8_t BSP_MPU6050_Check(void);

/* 14 字节突发读，返回 0=成功 */
uint8_t BSP_MPU6050_ReadRaw(mpu6050_raw_t *raw);

/* 零偏校准：静止采样 samples 次（建议 ~200 次 @200Hz），更新内部校准 */
void BSP_MPU6050_Calibrate(const uint16_t samples);

void BSP_MPU6050_GetCal(mpu6050_cal_t *cal);

#endif
