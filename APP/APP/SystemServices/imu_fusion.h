#ifndef IMU_FUSION_H
#define IMU_FUSION_H

/* ================================================================
 * Mahony AHRS 姿态解算（四元数互补滤波）
 *
 * 选型说明（为什么是 Mahony 而非 2D 卡尔曼/EKF）：
 *   - 六轴（无磁力计）场景下，Mahony 是工业界标准解
 *     （ArduPilot/大量飞控同源思想）：计算量极小、抗扰性强、
 *     横滚/俯仰由重力参考（无漂移、收敛快）；
 *   - 2D 卡尔曼只解横滚/俯仰且调参脆弱；完整 EKF 需 9 轴 +
 *     巨大算力，对六轴并无增益——Mahony 是精度/鲁棒/算力
 *     三重最优解。
 *   - 航向（Yaw）仅靠陀螺积分（无磁力计绝对参考），会缓慢漂移，
 *     属六轴物理极限；横滚/俯仰不受影响。
 *
 * 算法：比例+积分反馈修正陀螺 → 四元数微分积分 → 归一化 → 欧拉角。
 *   Kp：加速度计信任度（越大收敛越快，噪声越大）
 *   Ki：陀螺零偏漂移在线估计（积分反馈）
 * ================================================================ */

typedef struct {
    float q0, q1, q2, q3;     /* 单位四元数 */
    float roll, pitch, yaw;   /* 欧拉角（度） */
    float fb_x, fb_y, fb_z;   /* 积分反馈（陀螺零偏漂移估计，rad/s） */
} imu_fusion_t;

void IMU_Fusion_Init(imu_fusion_t *f);

/* 单步融合：陀螺 rad/s（已去零偏），加速度 g（已去偏移），dt 秒 */
void IMU_Fusion_Update(imu_fusion_t *f,
                       float gx, float gy, float gz,
                       float ax, float ay, float az,
                       float dt);

#endif
