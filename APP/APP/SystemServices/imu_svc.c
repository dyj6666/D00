/* ================================================================
 * imu_svc —— MPU6050 服务：采样/校准/姿态融合上报
 *
 * 架构位置：APP 服务层；独立 IMU 任务
 * 核心流程：I2C 读原始数据 -> 互补滤波 -> 事件总线广播
 * ================================================================ */
#include "imu_svc.h"
#include "bsp_mpu6050.h"
#include "ctrl/ctrl.h"     /* 互补滤波（对比通道） */
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "logger.h"

#include <math.h>
#include <stdio.h>

/* ================================================================
 * IMU 服务（工业级数据管线，卡尔曼最优估计）
 *
 * 架构：
 *   BSP(MPU6050) → 静态零偏校准 → 轴映射(X/Y对调)
 *   → 2D 卡尔曼（角度+陀螺零偏双状态，加速度计直接测量）
 *     ——六轴 IMU 倾角的最优线性估计器（经典 Kalman Tilt）
 *   → 偏航积分 + 静止漂移抑制 → 欧拉→四元数 → 共享状态
 *
 * 卡尔曼为什么是"最大化应用"：
 *   1. 双状态（角度 + 零偏）：陀螺零偏在运行中在线估计，
 *      优于仅启动校准的静态补偿；
 *   2. 加速度计直接作为测量（atan2 重力方向）：
 *      融合即最优，而非对已融合输出做后置平滑；
 *   3. 陀螺做预测（角速度积分 + 协方差传播）：
 *      快速运动保持响应，静止时收敛到加速度计参考。
 * ================================================================ */

#define IMU_TASK_STACK   2048   /* GCC + newlib 浮点打印（%+.3f）栈深明显大于
                                 * Keil 实测 HW 664B；1024B 在校准后打印时溢出
                                 * （实测 Stack Overflow 复位循环） */
#define IMU_SAMPLE_MS    5      /* 200Hz */
#define IMU_CAL_SAMPLES  200    /* 校准 ~1s（需静止） */
#define IMU_MAX_DT       0.05f  /* dt 上限（防调度饥饿时积分爆炸） */
#define RAD2DEG          57.29578f

/* 卡尔曼调参（rad 域，运动自适应） */
#define KF_Q_ANGLE      0.02f   /* 角度过程噪声（更高→更实时） */
#define KF_Q_BIAS       0.0002f /* 零偏过程噪声（慢变，防吸收运动） */
#define KF_R_BASE       0.02f   /* 静止测量噪声（信任加速度计，收敛稳定） */
#define KF_R_MOTION     0.5f    /* 随角速度增长的测量噪声（运动时信任陀螺，
                                 * 实时跟随无滞后） */

static imu_svc_state_t s_state;
static osThreadId_t s_task = NULL;

/* ---------- 2D 卡尔曼（角度 + 陀螺零偏双状态） ---------- */
typedef struct {
    float angle, bias;
    float P00, P01, P10, P11;
    float Q_angle, Q_bias, R_measure;
} kf2d_t;

static kf2d_t s_kf_roll, s_kf_pitch;
static float s_yaw_angle = 0.0f;   /* 偏航（陀螺积分，rad） */
static uint8_t s_kf_inited = 0;

/* ---------- 滤波对比通道（ctrl 库：互补滤波） ----------
 * 与 2D KF 同一数据流并行运行，供 GIMBAL 实验室同屏对比：
 *   原始加速度角度（毛刺） vs 互补（平滑但滞后） vs KF（最优） */
#define COMP_TAU_S   1.0f    /* 互补时间常数（秒）：越大越信任陀螺 */
static Complementary s_comp_roll, s_comp_pitch;

static void kf2d_init(kf2d_t *k, float qa, float qb, float rm)
{
    k->angle = 0.0f;
    k->bias = 0.0f;
    k->P00 = k->P01 = k->P10 = k->P11 = 0.0f;
    k->Q_angle = qa;
    k->Q_bias = qb;
    k->R_measure = rm;
}

static float kf2d_update(kf2d_t *k, float gyro, float meas, float dt)
{
    /* ---- 预测（陀螺积分 + 协方差传播） ---- */
    k->angle += (gyro - k->bias) * dt;
    k->P00 += dt * (dt * k->P11 - k->P01 - k->P10 + k->Q_angle);
    k->P01 -= dt * k->P11;
    k->P10 -= dt * k->P11;
    k->P11 += k->Q_bias * dt;

    /* ---- 更新（加速度计测量修正，最优增益） ---- */
    float y  = meas - k->angle;
    float S  = k->P00 + k->R_measure;
    float K0 = k->P00 / S;
    float K1 = k->P10 / S;
    k->angle += K0 * y;
    k->bias  += K1 * y;
    k->P00 -= K0 * k->P00;
    k->P01 -= K0 * k->P01;
    k->P10 -= K1 * k->P00;
    k->P11 -= K1 * k->P01;
    return k->angle;
}

/* ---------- 偏航漂移抑制：静止时在线估计 gz 零漂 ---------- */
static float s_yaw_bias = 0.0f;    /* rad/s */
static uint16_t s_still_n = 0;
static double s_gz_acc = 0.0;

/** @brief 浮点定点格式化：+123.456 写入 out（不依赖 %f，省 Flash） */
void ImuSvc_FormatFixed(float v, int dec, char *out)
{
    static const int32_t scale[4] = { 1, 10, 100, 1000 };
    int32_t m = (int32_t)(v * (float)scale[dec] + ((v < 0) ? -0.5f : 0.5f));
    uint32_t u = (uint32_t)((m < 0) ? -m : m);
    char *p = out;
    *p++ = (m < 0) ? '-' : '+';
    p += (size_t)sprintf(p, "%lu", (unsigned long)(u / (uint32_t)scale[dec]));
    *p++ = '.';
    for (int32_t d = scale[dec] / 10; d > 1 && (int32_t)(u % (uint32_t)scale[dec]) < d; d /= 10) {
        *p++ = '0';
    }
    p += (size_t)sprintf(p, "%lu",
                         (unsigned long)(u % (uint32_t)scale[dec]));
    *p = '\0';
}

/* ---------- 采样任务 ---------- */
static void imu_task(void *arg)
{
    (void)arg;
    uint32_t last_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint8_t warned = 0;

    /* 初始化失败不自暴自弃：驱动已带总线释放，周期重试自愈
     * （覆盖上电时序未稳 / I2C 从机钳位等瞬态） */
    while (BSP_MPU6050_Init() != 0) {
        s_state.fault_count++;
        LOG_Printf("[IMU] MPU6050 init FAIL (faults=%lu), retry in 2s...\r\n",
                   (unsigned long)s_state.fault_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    LOG_Printf("[IMU] MPU6050 ready (WHO_AM_I=0x68, I2C1 400kHz)\r\n");

    LOG_Printf("[IMU] calibrating gyro bias, keep board STILL...\r\n");
    BSP_MPU6050_Calibrate(IMU_CAL_SAMPLES);
    {
        mpu6050_cal_t cal;
        BSP_MPU6050_GetCal(&cal);
        char bx[16], by[16], bz[16];
        ImuSvc_FormatFixed(cal.gx_bias, 3, bx);
        ImuSvc_FormatFixed(cal.gy_bias, 3, by);
        ImuSvc_FormatFixed(cal.gz_bias, 3, bz);
        LOG_Printf("[IMU] cal gyro_bias=(%s,%s,%s) dps\r\n",
                   bx, by, bz);
    }

    kf2d_init(&s_kf_roll, KF_Q_ANGLE, KF_Q_BIAS, KF_R_BASE);
    kf2d_init(&s_kf_pitch, KF_Q_ANGLE, KF_Q_BIAS, KF_R_BASE);
    Complementary_Init(&s_comp_roll, COMP_TAU_S, 0.005f);
    Complementary_Init(&s_comp_pitch, COMP_TAU_S, 0.005f);
    s_state.ready = 1;

    /* 精确 200Hz 节拍：vTaskDelayUntil 消除累积抖动（dt 自适应仍兜底） */
    TickType_t xLastWake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        float dt = (float)(now - last_ms) * 0.001f;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > IMU_MAX_DT) dt = IMU_MAX_DT;
        last_ms = now;

        mpu6050_raw_t raw;
        if (BSP_MPU6050_ReadRaw(&raw) == 0) {
            mpu6050_cal_t cal;
            BSP_MPU6050_GetCal(&cal);

            float ax = (float)raw.ax / MPU6050_ACCEL_SCALE - cal.ax_off;
            float ay = (float)raw.ay / MPU6050_ACCEL_SCALE - cal.ay_off;
            float az = (float)raw.az / MPU6050_ACCEL_SCALE - cal.az_off;
            float gx = ((float)raw.gx / MPU6050_GYRO_SCALE - cal.gx_bias) * 0.0174533f;
            float gy = ((float)raw.gy / MPU6050_GYRO_SCALE - cal.gy_bias) * 0.0174533f;
            float gz = ((float)raw.gz / MPU6050_GYRO_SCALE - cal.gz_bias) * 0.0174533f;

            /* 传感器轴映射：X/Y 对调（R/P 与物理方向对齐） */
            float axm = ay, aym = ax, azm = az;
            float gxm = gy, gym = gx, gzm = gz;

            /* 静止检测 + 偏航零漂在线估计（映射后 gz） */
            {
                float amag = sqrtf(axm * axm + aym * aym + azm * azm);
                float gabs = fabsf(gxm) + fabsf(gym) + fabsf(gzm);
                if (fabsf(amag - 1.0f) < 0.06f && gabs < 0.25f) {
                    s_still_n++;
                    s_gz_acc += gzm;
                    if (s_still_n >= 100) {
                        s_yaw_bias = (float)(s_gz_acc / s_still_n);
                        s_still_n = 0;
                        s_gz_acc = 0.0;
                    }
                } else {
                    s_still_n = 0;
                    s_gz_acc = 0.0;
                }
                gzm -= s_yaw_bias;   /* 扣除偏航漂移 */
            }

            /* 2D 卡尔曼：加速度计直接测量（最优倾角估计） */
            if (!s_kf_inited) {
                s_kf_roll.angle = atan2f(aym, azm);
                s_kf_pitch.angle = atan2f(-axm, sqrtf(aym * aym + azm * azm));
                s_kf_inited = 1;
            }
            /* 运动自适应测量噪声：静止低 R（跟随加速度计）、
             * 运动高 R（紧跟陀螺预测）→ 实时性与平稳性兼得 */
            {
                float gyro_mag = fabsf(gxm) + fabsf(gym);
                float r_meas = KF_R_BASE + KF_R_MOTION * gyro_mag;
                s_kf_roll.R_measure = r_meas;
                s_kf_pitch.R_measure = r_meas;
            }
            float roll  = kf2d_update(&s_kf_roll,
                                      gxm, atan2f(aym, azm), dt);
            float pitch = kf2d_update(&s_kf_pitch,
                                      gym,
                                      atan2f(-axm,
                                             sqrtf(aym * aym + azm * azm)),
                                      dt);
            s_yaw_angle += gzm * dt;

            /* ---- 滤波对比通道（同数据流并行） ---- */
            float acc_r = atan2f(aym, azm);          /* 加速度原始角度（rad） */
            float acc_p = atan2f(-axm,
                                 sqrtf(aym * aym + azm * azm));
            float comp_r = Complementary_Update(&s_comp_roll, gxm, acc_r);
            float comp_p = Complementary_Update(&s_comp_pitch, gym, acc_p);
            /* 显示约定与 KF 通道一致（镜像） */
            s_state.acc_roll  = -acc_r  * RAD2DEG;
            s_state.acc_pitch = -acc_p  * RAD2DEG;
            s_state.comp_roll = -comp_r * RAD2DEG;
            s_state.comp_pitch = -comp_p * RAD2DEG;

            /* 显示约定：镜像（延续用户确认的 R/P 方向） */
            float rd = -roll * RAD2DEG;
            float pd = -pitch * RAD2DEG;
            float yd = s_yaw_angle * RAD2DEG;

            /* 欧拉 → 四元数（立方体，与显示一致；镜像= q1/q2 取反） */
            float cy = cosf(s_yaw_angle * 0.5f);
            float sy = sinf(s_yaw_angle * 0.5f);
            /* 立方体 P 方向修正：与显示约定反向（用户确认） */
            float cp = cosf(-pitch * 0.5f);
            float sp = sinf(-pitch * 0.5f);
            float cr = cosf(roll * 0.5f);
            float sr = sinf(roll * 0.5f);
            float q0 = cy * cp * cr + sy * sp * sr;
            float q1 = cy * cp * sr - sy * sp * cr;
            float q2 = cy * sp * cr + sy * cp * sr;
            float q3 = sy * cp * cr - cy * sp * sr;

            s_state.q0 = q0;
            s_state.q1 = -q1;
            s_state.q2 = -q2;
            s_state.q3 = q3;
            s_state.roll = rd;
            s_state.pitch = pd;
            s_state.yaw = yd;
            s_state.ax = axm; s_state.ay = aym; s_state.az = azm;
            s_state.gx = gxm * RAD2DEG;
            s_state.gy = gym * RAD2DEG;
            s_state.gz = gzm * RAD2DEG;   /* 已扣偏航漂移 */
            s_state.temp = (float)raw.temp / MPU6050_TEMP_SCALE + MPU6050_TEMP_OFFSET;
            s_state.sample_count++;
            warned = 0;
        } else if (!warned) {
            s_state.fault_count++;
            warned = 1;
            LOG_Printf("[IMU] read fault %lu\r\n",
                       (unsigned long)s_state.fault_count);
        }
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(IMU_SAMPLE_MS));
    }
}

/* ---------- 接口 ---------- */
void ImuSvc_Init(void)
{
    if (s_task != NULL) return;

    osThreadAttr_t attr = {
        .name = "ImuSvc",
        .stack_size = IMU_TASK_STACK,
        .priority = osPriorityAboveNormal,
    };
    s_task = osThreadNew(imu_task, NULL, &attr);
}

const imu_svc_state_t *ImuSvc_GetState(void)
{
    return &s_state;
}

void ImuSvc_Recalibrate(void)
{
    LOG_Printf("[IMU] recalibrating, keep board STILL & level...\r\n");
    BSP_MPU6050_Calibrate(IMU_CAL_SAMPLES);
    mpu6050_cal_t cal;
    BSP_MPU6050_GetCal(&cal);
    LOG_Printf("[IMU] recal done gyro_bias=(%+.3f,%+.3f,%+.3f) dps\r\n",
               cal.gx_bias, cal.gy_bias, cal.gz_bias);
}
