/* ================================================================
 * kalman.h —— 卡尔曼/贝叶斯滤波全家族（15 变式）
 *
 * 架构位置：APP 应用层 ctrl/ 子模块；纯 C、float 运算、
 *           结构体实例化（可重入、多实例、无动态内存）。
 *
 * 知识定位（贝叶斯滤波谱系）：
 *   经典 Kalman（线性高斯） ← 最基础
 *     ├── 1D 标量 / 2D 角度+bias（工程最常见）
 *     ├── α-β / α-β-γ（KF 恒定速度/加速度模型的简化特例）
 *     ├── 信息滤波（协方差逆形式，多传感器融合友好）
 *     └── 平方根滤波（数值稳定性，大动态范围）
 *   扩展 Kalman（EKF：非线性一阶线性化）
 *   无迹 Kalman（UKF：sigma 点无迹变换，强非线性）
 *   交互式多模型（IMM：多模型加权，机动目标）
 *   自适应 Kalman（创新序列协方差匹配，Q/R 在线估计）
 *   RTS 平滑器（离线后向校正）
 *   粒子滤波（PF：蒙特卡洛重采样，非高斯）
 *   互补滤波 / Mahony（AHRS 姿态专用，工程实用主义）
 *
 * 使用场景索引：
 *   MPU6050 姿态（陀螺+加速度）→ [03]kf_2d / [06]complementary / [07]mahony
 *   视觉目标平滑/预测 → [04]α-β / [02]kf_1d
 *   云台角速度估计 → [03]kf_2d
 *   机动目标跟踪 → [11]IMM
 *   噪声时变环境 → [10]自适应
 *   离线数据分析 → [09]RTS
 *   强非线性（大角度姿态）→ [08]UKF / [07]EKF
 * ================================================================ */
#ifndef KALMAN_H
#define KALMAN_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * [01] 通用多维卡尔曼滤波器（固定 4 维上限，栈内矩阵）
 *
 *  预测：x = F·x；P = F·P·Fᵀ + Q
 *  更新：K = P·Hᵀ(S)⁻¹, S = H·P·Hᵀ + R
 *        x = x + K·(z - H·x)；P = (I - K·H)·P
 *
 * 场景：任意线性状态估计（位置/速度/温度/电压）；教学基准。
 * 限制：状态维度 ≤ KF_MAX_DIM（默认 4）；矩阵为普通 float 数组。
 * ---------------------------------------------------------------- */
#define KF_MAX_DIM   4

typedef struct {
    int32_t n;                  /* 状态维数 */
    float F[KF_MAX_DIM][KF_MAX_DIM];  /* 状态转移矩阵 */
    float H[KF_MAX_DIM][KF_MAX_DIM];  /* 观测矩阵（行数=观测维 ≤ n） */
    float Q[KF_MAX_DIM][KF_MAX_DIM];  /* 过程噪声协方差 */
    float R[KF_MAX_DIM][KF_MAX_DIM];  /* 观测噪声协方差（观测维） */
    float P[KF_MAX_DIM][KF_MAX_DIM];  /* 估计误差协方差（内部） */
    float x[KF_MAX_DIM];       /* 状态向量（读） */
    int32_t m;                  /* 观测维数 */
} KF_Generic;

void KF_Generic_Init(KF_Generic *k);
/* z：观测向量（m 维）；更新一步，状态写入 k->x */
void KF_Generic_Update(KF_Generic *k, const float *z);

/* ----------------------------------------------------------------
 * [02] 一维标量卡尔曼（教学/单变量）
 *
 *  恒定状态模型（可带速度扩展）：
 *  场景：单传感器平滑（电压/温度/距离），理解 KF 首选。
 * ---------------------------------------------------------------- */
typedef struct {
    float q;                /* 过程噪声方差 */
    float r;                /* 测量噪声方差 */
    float x;                /* 状态估计（读） */
    float p;                /* 估计方差（读） */
    float k;                /* 卡尔曼增益（读，教学） */
} KF_1D;

void KF_1D_Init(KF_1D *k, float q, float r, float x0, float p0);
float KF_1D_Update(KF_1D *k, float z);

/* ----------------------------------------------------------------
 * [03] 二维卡尔曼（角度 + 陀螺零偏）——IMU 姿态经典
 *
 *  状态 x = [angle, bias]；输入 = 陀螺角速度（gyro - bias 积分），
 *  观测 = 加速度计解算角度。
 *  场景：MPU6050 云台/平衡车姿态（imu_svc 现有 kf2d 的同源升级版）。
 * ---------------------------------------------------------------- */
typedef struct {
    float q_angle;          /* 角度过程噪声 */
    float q_bias;           /* 零偏过程噪声 */
    float r_measure;        /* 测量噪声 */
    float angle;            /* 角度估计（读） */
    float bias;             /* 零偏估计（读） */
    float p[2][2];          /* 协方差（读，教学） */
    float dt;               /* 采样周期（秒） */
} KF_2D;

void KF_2D_Init(KF_2D *k, float q_angle, float q_bias, float r_measure);
/* gyro：角速度（rad/s）；meas：加速度计角度（rad） */
float KF_2D_Update(KF_2D *k, float gyro, float meas);

/* ----------------------------------------------------------------
 * [04] α-β 滤波器（KF 恒定速度模型特例）
 *
 *  位置 + 速度两个状态，增益固定为 α/β（非自适应）：
 *  场景：雷达/视觉目标跟踪的轻量平滑与速度估计——
 *        OpenART 目标像素坐标平滑的首选。
 * ---------------------------------------------------------------- */
typedef struct {
    float alpha, beta;      /* 固定增益（α∈(0,1]，β 通常 α²/(2-α)） */
    float dt;
    float pos, vel;         /* 位置/速度估计（读） */
} AB_Filter;

void AB_Filter_Init(AB_Filter *k, float alpha, float beta, float dt);
float AB_Filter_Update(AB_Filter *k, float z_pos);

/* ----------------------------------------------------------------
 * [05] α-β-γ 滤波器（恒定加速度模型）
 *
 *  位置/速度/加速度三状态，增益 α/β/γ 固定。
 *  场景：高机动目标平滑（目标加速度显著时优于 α-β）。
 * ---------------------------------------------------------------- */
typedef struct {
    float alpha, beta, gamma;
    float dt;
    float pos, vel, acc;
} ABG_Filter;

void ABG_Filter_Init(ABG_Filter *k, float alpha, float beta, float gamma, float dt);
float ABG_Filter_Update(ABG_Filter *k, float z_pos);

/* ----------------------------------------------------------------
 * [06] 互补滤波（陀螺 + 加速度）
 *
 *  angle = α·(angle + gyro·dt) + (1-α)·accel_angle
 *  α 按时间常数 τ 选取：α = τ/(τ+dt)。
 *  场景：姿态估计的工程务实基线（计算量最小、无协方差）。
 * ---------------------------------------------------------------- */
typedef struct {
    float tau;              /* 时间常数（秒，通常 0.5~2） */
    float dt;
    float angle;            /* 融合角度（读） */
} Complementary;

void Complementary_Init(Complementary *k, float tau, float dt);
float Complementary_Update(Complementary *k, float gyro, float accel_angle);

/* ----------------------------------------------------------------
 * [07] Mahony 互补滤波（AHRS 四元数姿态）
 *
 *  基于四元数的显式互补滤波：加速度计/磁力计作为"参考向量"，
 *  比例-积分修正陀螺零偏。无需矩阵求逆，嵌入式 AHRS 经典。
 *  场景：MPU6050 + 磁力计的全姿态（roll/pitch/yaw）。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp, ki;           /* 修正增益（kp≈0.5, ki≈0.05 起步） */
    float q0, q1, q2, q3;   /* 姿态四元数（读） */
    float ex, ey, ez;       /* 积分误差累积（内部） */
    float dt;
} Mahony;

void Mahony_Init(Mahony *k, float kp, float ki);
/* gx,gy,gz：陀螺（rad/s）；ax,ay,az：加速度（归一化可选） */
void Mahony_Update(Mahony *k, float gx, float gy, float gz,
                   float ax, float ay, float az);
/* 四元数 → 欧拉角（roll/pitch/yaw，rad） */
void Mahony_ToEuler(const Mahony *k, float *roll, float *pitch, float *yaw);

/* ----------------------------------------------------------------
 * [08] 扩展卡尔曼（EKF：非线性系统通用框架）
 *
 *  用户提供：状态转移 f(x)、观测 h(x)、对应雅可比 F/H（或数值差分）。
 *  本实现内置数值差分雅可比（无需手推导数），代价略高。
 *  场景：四元数姿态、里程计、任何非线性状态估计。
 * ---------------------------------------------------------------- */
#define EKF_MAX_DIM   4

typedef struct {
    int32_t n, m;                   /* 状态/观测维数 */
    float x[EKF_MAX_DIM];           /* 状态（读/写） */
    float P[EKF_MAX_DIM][EKF_MAX_DIM]; /* 协方差（内部） */
    float Q[EKF_MAX_DIM][EKF_MAX_DIM];
    float R[EKF_MAX_DIM][EKF_MAX_DIM];
    /* 用户回调：f(x) 写入 x_next；h(x) 写入 z_pred */
    void (*f)(const float *x, float *x_next, void *ctx);
    void (*h)(const float *x, float *z_pred, void *ctx);
    void *ctx;
    float dt;               /* 数值差分步长（默认 1e-4） */
} EKF;

void EKF_Init(EKF *k);
void EKF_Update(EKF *k, const float *z);

/* ----------------------------------------------------------------
 * [09] 无迹卡尔曼（UKF：sigma 点变换）
 *
 *  2n+1 个 sigma 点经非线性函数传播，统计矩重建均值/协方差——
 *  对强非线性优于 EKF 一阶近似，无需雅可比。
 *  场景：大角度姿态、航迹推算、传感器标定。
 * ---------------------------------------------------------------- */
#define UKF_MAX_DIM   3

typedef struct {
    int32_t n, m;
    float x[UKF_MAX_DIM];
    float P[UKF_MAX_DIM][UKF_MAX_DIM];
    float Q[UKF_MAX_DIM][UKF_MAX_DIM];
    float R[UKF_MAX_DIM][UKF_MAX_DIM];
    float alpha, beta, kappa;   /* 尺度参数（默认 1e-3/2/0） */
    void (*f)(const float *x, float *x_next, void *ctx);
    void (*h)(const float *x, float *z_pred, void *ctx);
    void *ctx;
} UKF;

void UKF_Init(UKF *k);
void UKF_Update(UKF *k, const float *z);

/* ----------------------------------------------------------------
 * [10] 自适应卡尔曼（创新序列协方差匹配）
 *
 *  用观测新息 ν = z - Hx 的实际协方差在线修正 R（测量噪声）——
 *  噪声统计未知/时变时的工程利器。
 *  场景：传感器噪声随环境变化（振动、光照、温度）。
 * ---------------------------------------------------------------- */
typedef struct {
    float q, r;             /* 过程/测量噪声（r 被在线修正） */
    float x, p;
    float nu;               /* 新息（读，教学） */
    float nu_var_est;       /* 新息方差滑动估计（读） */
    float nu_var_alpha;     /* 滑动平均系数（默认 0.05） */
    float r_min;            /* R 下限保护（防负） */
    uint32_t cnt;
} KF_Adaptive;

void KF_Adaptive_Init(KF_Adaptive *k, float q, float r0);
float KF_Adaptive_Update(KF_Adaptive *k, float z);

/* ----------------------------------------------------------------
 * [11] 交互式多模型（IMM：2~3 个卡尔曼加权）
 *
 *  多个模型（如：匀速/匀加速/机动）并行运行，按模型似然
 *  动态加权输出——机动目标跟踪的"最优解"工程化。
 *  场景：目标突然变速/变向（视觉跟踪、雷达）。
 * ---------------------------------------------------------------- */
#define IMM_MAX_MODELS   3

typedef struct {
    int32_t n_models;               /* 模型数（2~3） */
    float mu[IMM_MAX_MODELS];       /* 模型概率（读） */
    float trans[IMM_MAX_MODELS][IMM_MAX_MODELS]; /* 马尔可夫转移矩阵 */
    /* 每模型：一维卡尔曼（位置）——工程简化版 */
    struct {
        float x, p;
        float q;                    /* 各模型过程噪声不同（机动模型大） */
        float r;
        float like;                 /* 模型似然 */
    } model[IMM_MAX_MODELS];
    float x_out, p_out;             /* 加权融合输出 */
} IMM;

void IMM_Init(IMM *k, int32_t n_models, const float *q_list, float r,
              const float *trans, const float *mu0);
float IMM_Update(IMM *k, float z);

/* ----------------------------------------------------------------
 * [12] 信息滤波（KF 的协方差逆形式）
 *
 *  状态信息 Y = P⁻¹，信息向量 y = P⁻¹x——更新方程变为加法，
 *  多传感器/多观测融合天然并行。场景：分布式/多源融合教学。
 * ---------------------------------------------------------------- */
typedef struct {
    float y[KF_MAX_DIM];            /* 信息向量（读） */
    float Y[KF_MAX_DIM][KF_MAX_DIM];/* 信息矩阵（读） */
    float F[KF_MAX_DIM][KF_MAX_DIM];
    float H[KF_MAX_DIM][KF_MAX_DIM];
    float Q[KF_MAX_DIM][KF_MAX_DIM];
    float R[KF_MAX_DIM][KF_MAX_DIM];
    int32_t n, m;
} InfoKF;

void InfoKF_Init(InfoKF *k);
void InfoKF_Update(InfoKF *k, const float *z);

/* ----------------------------------------------------------------
 * [13] 平方根卡尔曼（Joseph 形式协方差更新）
 *
 *  协方差用平方根 S（P = S·Sᵀ）传播，保证半正定、
 *  抑制舍入误差——大动态范围/低精度平台（float）的稳定性之选。
 *  场景：长时间运行、数值敏感的状态估计。
 * ---------------------------------------------------------------- */
typedef struct {
    float q, r;
    float x, p_sqrt;        /* p_sqrt = sqrt(P)（读） */
} KF_Sqrt;

void KF_Sqrt_Init(KF_Sqrt *k, float q, float r);
float KF_Sqrt_Update(KF_Sqrt *k, float z);

/* ----------------------------------------------------------------
 * [14] RTS 平滑器（离线后向校正）
 *
 *  前向 KF + 后向平滑：利用"未来"数据改善历史估计——
 *  离线回放/数据分析的标准工具。场景：轨迹后处理、算法对比基准。
 * ---------------------------------------------------------------- */
#define RTS_MAX_LEN   256

typedef struct {
    float q, r;
    float xf[RTS_MAX_LEN], pf[RTS_MAX_LEN]; /* 前向估计存储 */
    float x[RTS_MAX_LEN];                   /* 平滑结果（读） */
    int32_t len;                            /* 已存点数 */
} RTS;

void RTS_Init(RTS *k, float q, float r);
void RTS_Add(RTS *k, float z);              /* 前向逐点喂入 */
void RTS_Smooth(RTS *k);                    /* 完成后调用，结果写 x[] */

/* ----------------------------------------------------------------
 * [15] 粒子滤波（PF：蒙特卡洛重采样）
 *
 *  粒子集合 + 权重更新 + 系统重采样——任意分布/强非线性。
 *  场景：非高斯噪声、多峰分布（室内定位）、教学对比。
 *  注：嵌入式上计算量大，N=64 起步。
 * ---------------------------------------------------------------- */
#define PF_PARTICLES   64

typedef struct {
    float x[PF_PARTICLES];  /* 粒子位置 */
    float w[PF_PARTICLES];  /* 粒子权重（归一化） */
    float q, r;             /* 过程/测量噪声 */
    float estimate;         /* 加权均值估计（读） */
    int32_t n;              /* 活跃粒子数 */
} PF;

void PF_Init(PF *k, float q, float r, int32_t n);
void PF_Update(PF *k, float z);
void PF_Resample(PF *k);

/* ================================================================
 * 通用数学辅助
 * ================================================================ */
static inline float kalman_clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline float kalman_sqrf(float v) { return v * v; }

#ifdef __cplusplus
}
#endif

#endif /* KALMAN_H */
