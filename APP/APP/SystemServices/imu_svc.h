/* ================================================================
 * imu_svc —— MPU6050 服务：姿态融合与数据上报
 *
 * 架构位置：APP 服务层；IMU 任务与融合算法封装
 * ================================================================ */
#ifndef IMU_SVC_H
#define IMU_SVC_H

#include <stdint.h>

/* ================================================================
 * IMU 服务（工业级数据管线）
 *   BSP(MPU6050) → 零偏校准 → Mahony AHRS 融合 → 共享状态
 *   - 200Hz 采样（I2C1 400kHz 14 字节突发读）
 *   - 启动陀螺零偏校准 + Mahony Ki 在线漂移补偿
 *   - 共享状态供 UI/上位机轮询（无队列洪泛）
 * ================================================================ */

typedef struct {
    volatile float q0, q1, q2, q3;      /* 单位四元数 */
    volatile float roll, pitch, yaw;    /* 欧拉角（度） */
    volatile float ax, ay, az;          /* 加速度（g，已去偏移） */
    volatile float gx, gy, gz;          /* 角速度（dps，已去零偏） */
    volatile float temp;                /* 温度（℃） */
    volatile uint32_t sample_count;     /* 累计有效采样 */
    volatile uint32_t fault_count;      /* I2C/器件故障计数 */
    volatile uint8_t  ready;            /* 1=器件就绪（含校准完成） */
} imu_svc_state_t;

void ImuSvc_Init(void);
const imu_svc_state_t *ImuSvc_GetState(void);

/* 在线重校准（需静止、尽量水平；阻塞执行 ~1s） */
void ImuSvc_Recalibrate(void);

#endif
