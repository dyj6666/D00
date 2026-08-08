#include "imu_fusion.h"

#include <math.h>
#include <stddef.h>

/* ================================================================
 * Mahony AHRS 实现（参考 Madgwick/Mahony 公开算法）
 * ================================================================ */

#define Kp    0.5f     /* 比例增益：加速度计信任度 */
#define Ki    0.05f    /* 积分增益：在线零偏漂移补偿 */
#define RAD2DEG  57.29578f

void IMU_Fusion_Init(imu_fusion_t *f)
{
    if (f == NULL) return;
    f->q0 = 1.0f;
    f->q1 = f->q2 = f->q3 = 0.0f;
    f->roll = f->pitch = f->yaw = 0.0f;
    f->fb_x = f->fb_y = f->fb_z = 0.0f;
}

void IMU_Fusion_Update(imu_fusion_t *f,
                       float gx, float gy, float gz,
                       float ax, float ay, float az,
                       float dt)
{
    if (f == NULL || dt <= 0.0f) return;

    float q0 = f->q0, q1 = f->q1, q2 = f->q2, q3 = f->q3;

    /* ---- 归一化加速度 ---- */
    float norm = sqrtf(ax * ax + ay * ay + az * az);
    if (norm < 1e-6f) norm = 1e-6f;
    ax /= norm;
    ay /= norm;
    az /= norm;

    /* ---- 四元数估计的重力方向 ---- */
    float vx = 2.0f * (q1 * q3 - q0 * q2);
    float vy = 2.0f * (q0 * q1 + q2 * q3);
    float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

    /* ---- 测量与估计的叉积误差（姿态差） ---- */
    float ex = ay * vz - az * vy;
    float ey = az * vx - ax * vz;
    float ez = ax * vy - ay * vx;

    /* ---- 积分反馈（零偏漂移补偿） ---- */
    f->fb_x += Ki * ex * dt;
    f->fb_y += Ki * ey * dt;
    f->fb_z += Ki * ez * dt;

    /* ---- 陀螺修正：比例 + 积分 ---- */
    gx += Kp * ex + f->fb_x;
    gy += Kp * ey + f->fb_y;
    gz += Kp * ez + f->fb_z;

    /* ---- 四元数微分方程（一阶积分） ---- */
    float dq0 = (-q1 * gx - q2 * gy - q3 * gz) * 0.5f * dt;
    float dq1 = ( q0 * gx + q2 * gz - q3 * gy) * 0.5f * dt;
    float dq2 = ( q0 * gy - q1 * gz + q3 * gx) * 0.5f * dt;
    float dq3 = ( q0 * gz + q1 * gy - q2 * gx) * 0.5f * dt;
    q0 += dq0;
    q1 += dq1;
    q2 += dq2;
    q3 += dq3;

    /* ---- 归一化 ---- */
    norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    if (norm < 1e-10f) norm = 1e-10f;
    f->q0 = q0 / norm;
    f->q1 = q1 / norm;
    f->q2 = q2 / norm;
    f->q3 = q3 / norm;

    /* ---- 欧拉角（度），asin 参数钳位防 NaN ---- */
    f->roll  = atan2f(2.0f * (f->q0 * f->q1 + f->q2 * f->q3),
                      1.0f - 2.0f * (f->q1 * f->q1 + f->q2 * f->q2)) * RAD2DEG;
    float sp = 2.0f * (f->q0 * f->q2 - f->q3 * f->q1);
    if (sp > 1.0f) sp = 1.0f;
    if (sp < -1.0f) sp = -1.0f;
    f->pitch = asinf(sp) * RAD2DEG;
    f->yaw   = atan2f(2.0f * (f->q0 * f->q3 + f->q1 * f->q2),
                      1.0f - 2.0f * (f->q2 * f->q2 + f->q3 * f->q3)) * RAD2DEG;
}
