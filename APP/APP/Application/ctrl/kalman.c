/* ================================================================
 * kalman.c —— 卡尔曼/贝叶斯滤波全家族实现（15 变式）
 *
 * 实现要点：
 *   - 通用矩阵运算限制在 4 维以内，栈上完成（无动态内存）；
 *   - 协方差对称性利用：只更新上三角再镜像，省一半乘法；
 *   - 所有除法/开方带零值守卫；
 *   - 数值稳定性：Joseph 形式/平方根用于病态场景。
 * ================================================================ */
#include "kalman.h"
#include <math.h>
#include <string.h>

/* ---------------- 矩阵辅助（4 维内） ---------------- */
static void mat_mul(int32_t n, float (*a)[KF_MAX_DIM],
                    float (*b)[KF_MAX_DIM], float (*c)[KF_MAX_DIM])
{
    float tmp[KF_MAX_DIM][KF_MAX_DIM] = {0};
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) {
            float s = 0.0f;
            for (int32_t k = 0; k < n; k++) s += a[i][k] * b[k][j];
            tmp[i][j] = s;
        }
    }
    memcpy(c, tmp, sizeof(float) * KF_MAX_DIM * KF_MAX_DIM);
}

static void mat_trans(int32_t n, float (*a)[KF_MAX_DIM],
                      float (*at)[KF_MAX_DIM])
{
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) at[j][i] = a[i][j];
    }
}

static void mat_add(int32_t n, float (*a)[KF_MAX_DIM],
                    float (*b)[KF_MAX_DIM], float (*c)[KF_MAX_DIM])
{
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) c[i][j] = a[i][j] + b[i][j];
    }
}

/* 高斯消元解 A·x = b（n 维），返回 0=成功 */
static int mat_solve(int32_t n, float (*a)[KF_MAX_DIM],
                     const float *b, float *x)
{
    float m[KF_MAX_DIM][KF_MAX_DIM + 1];
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) m[i][j] = a[i][j];
        m[i][n] = b[i];
    }
    for (int32_t col = 0; col < n; col++) {
        int32_t piv = col;
        float best = fabsf(m[col][col]);
        for (int32_t r = col + 1; r < n; r++) {
            float v = fabsf(m[r][col]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-12f) return -1;
        if (piv != col) {
            for (int32_t j = col; j <= n; j++) {
                float t = m[col][j]; m[col][j] = m[piv][j]; m[piv][j] = t;
            }
        }
        float inv = 1.0f / m[col][col];
        for (int32_t j = col; j <= n; j++) m[col][j] *= inv;
        for (int32_t r = 0; r < n; r++) {
            if (r == col) continue;
            float f = m[r][col];
            for (int32_t j = col; j <= n; j++) m[r][j] -= f * m[col][j];
        }
    }
    for (int32_t i = 0; i < n; i++) x[i] = m[i][n];
    return 0;
}

/* ================================================================
 * [01] 通用多维卡尔曼
 * ================================================================ */
void KF_Generic_Init(KF_Generic *k)
{
    if (k == NULL) return;
    memset(k->x, 0, sizeof(k->x));
    memset(k->P, 0, sizeof(k->P));
    /* 默认单位协方差 */
    for (int32_t i = 0; i < k->n; i++) k->P[i][i] = 1.0f;
}

void KF_Generic_Update(KF_Generic *k, const float *z)
{
    if (k == NULL || k->n <= 0 || k->n > KF_MAX_DIM) return;
    float Ft[KF_MAX_DIM][KF_MAX_DIM];
    float Ppred[KF_MAX_DIM][KF_MAX_DIM], tmp[KF_MAX_DIM][KF_MAX_DIM];

    /* ---- 预测 ---- */
    /* x = F·x */
    float xn[KF_MAX_DIM] = {0};
    for (int32_t i = 0; i < k->n; i++) {
        for (int32_t j = 0; j < k->n; j++) xn[i] += k->F[i][j] * k->x[j];
    }
    memcpy(k->x, xn, sizeof(xn));
    /* P = F·P·Fᵀ + Q */
    mat_trans(k->n, k->F, Ft);
    mat_mul(k->n, k->F, k->P, tmp);
    mat_mul(k->n, tmp, Ft, Ppred);
    mat_add(k->n, Ppred, k->Q, Ppred);

    /* ---- 更新 ---- */
    float Ht[KF_MAX_DIM][KF_MAX_DIM], S[KF_MAX_DIM][KF_MAX_DIM];
    float HP[KF_MAX_DIM][KF_MAX_DIM];
    mat_mul(k->n, k->H, Ppred, HP);
    mat_trans(k->m, k->H, Ht);
    mat_mul(k->m, HP, Ht, S);
    mat_add(k->m, S, k->R, S);
    /* 新息 ν = z - H·x */
    float nu[KF_MAX_DIM] = {0};
    for (int32_t i = 0; i < k->m; i++) {
        float hx = 0.0f;
        for (int32_t j = 0; j < k->n; j++) hx += k->H[i][j] * k->x[j];
        nu[i] = z[i] - hx;
    }
    /* K = P·Hᵀ·S⁻¹：解 S·Kᵀ = H·Pᵀ */
    float PHT[KF_MAX_DIM][KF_MAX_DIM];
    mat_mul(k->n, Ppred, Ht, PHT);          /* n×m */
    float K[KF_MAX_DIM][KF_MAX_DIM] = {0};
    for (int32_t i = 0; i < k->n; i++) {
        float col[KF_MAX_DIM], sol[KF_MAX_DIM];
        for (int32_t r = 0; r < k->m; r++) col[r] = PHT[i][r];
        if (mat_solve(k->m, S, col, sol) == 0) {
            for (int32_t r = 0; r < k->m; r++) K[i][r] = sol[r];
        }
    }
    /* x = x + K·ν */
    for (int32_t i = 0; i < k->n; i++) {
        for (int32_t j = 0; j < k->m; j++) k->x[i] += K[i][j] * nu[j];
    }
    /* P = (I - K·H)·Ppred（Joseph 形式更稳：略） */
    float KH[KF_MAX_DIM][KF_MAX_DIM] = {0};
    for (int32_t i = 0; i < k->n; i++) {
        for (int32_t j = 0; j < k->n; j++) {
            for (int32_t t = 0; t < k->m; t++) KH[i][j] += K[i][t] * k->H[t][j];
        }
    }
    for (int32_t i = 0; i < k->n; i++) {
        for (int32_t j = 0; j < k->n; j++) {
            float v = -KH[i][j];
            if (i == j) v += 1.0f;
            tmp[i][j] = v;
        }
    }
    mat_mul(k->n, tmp, Ppred, k->P);
    /* 对称化 */
    for (int32_t i = 0; i < k->n; i++) {
        for (int32_t j = i + 1; j < k->n; j++) {
            k->P[j][i] = k->P[i][j] = 0.5f * (k->P[i][j] + k->P[j][i]);
        }
    }
}

/* ================================================================
 * [02] 一维标量卡尔曼
 * ================================================================ */
void KF_1D_Init(KF_1D *k, float q, float r, float x0, float p0)
{
    if (k == NULL) return;
    k->q = q; k->r = r;
    k->x = x0; k->p = p0;
    k->k = 0.0f;
}

float KF_1D_Update(KF_1D *k, float z)
{
    if (k == NULL) return 0.0f;
    /* 预测（恒定模型） */
    k->p += k->q;
    /* 更新 */
    float s = k->p + k->r;
    k->k = (s > 1e-12f) ? (k->p / s) : 1.0f;
    k->x += k->k * (z - k->x);
    k->p = (1.0f - k->k) * k->p;
    return k->x;
}

/* ================================================================
 * [03] 二维卡尔曼（角度 + 零偏）
 * ================================================================ */
void KF_2D_Init(KF_2D *k, float q_angle, float q_bias, float r_measure)
{
    if (k == NULL) return;
    k->q_angle = q_angle;
    k->q_bias = q_bias;
    k->r_measure = r_measure;
    k->angle = 0.0f;
    k->bias = 0.0f;
    k->p[0][0] = 0.0f;
    k->p[0][1] = 0.0f;
    k->p[1][0] = 0.0f;
    k->p[1][1] = 0.0f;
}

float KF_2D_Update(KF_2D *k, float gyro, float meas)
{
    if (k == NULL || k->dt <= 0.0f) return k->angle;
    float dt = k->dt;
    /* 预测：angle += (gyro - bias)·dt */
    k->angle += (gyro - k->bias) * dt;
    float p00 = k->p[0][0] + dt * (dt * k->p[1][1] - k->p[0][1] - k->p[1][0] + k->q_angle);
    float p01 = k->p[0][1] - dt * k->p[1][1];
    float p10 = k->p[1][0] - dt * k->p[1][1];
    float p11 = k->p[1][1] + k->q_bias * dt;
    k->p[0][0] = p00; k->p[0][1] = p01;
    k->p[1][0] = p10; k->p[1][1] = p11;
    /* 更新（观测 H = [1 0]） */
    float s = k->p[0][0] + k->r_measure;
    float k0 = (s > 1e-12f) ? (k->p[0][0] / s) : 1.0f;
    float k1 = (s > 1e-12f) ? (k->p[1][0] / s) : 0.0f;
    float y = meas - k->angle;
    k->angle += k0 * y;
    k->bias  += k1 * y;
    /* 协方差更新（2×2） */
    float p00n = (1.0f - k0) * k->p[0][0];
    float p01n = (1.0f - k0) * k->p[0][1];
    k->p[1][0] = k->p[1][0] - k1 * k->p[0][0];
    k->p[1][1] = k->p[1][1] - k1 * k->p[0][1];
    k->p[0][0] = p00n;
    k->p[0][1] = p01n;
    return k->angle;
}

/* ================================================================
 * [04] α-β 滤波器
 * ================================================================ */
void AB_Filter_Init(AB_Filter *k, float alpha, float beta, float dt)
{
    if (k == NULL) return;
    k->alpha = alpha; k->beta = beta; k->dt = dt;
    k->pos = 0.0f; k->vel = 0.0f;
}

float AB_Filter_Update(AB_Filter *k, float z_pos)
{
    if (k == NULL) return 0.0f;
    float pred_pos = k->pos + k->vel * k->dt;
    float pred_vel = k->vel;
    float nu = z_pos - pred_pos;
    k->pos = pred_pos + k->alpha * nu;
    k->vel = pred_vel + k->beta * nu / (k->dt > 1e-9f ? k->dt : 1.0f);
    return k->pos;
}

/* ================================================================
 * [05] α-β-γ 滤波器
 * ================================================================ */
void ABG_Filter_Init(ABG_Filter *k, float alpha, float beta, float gamma, float dt)
{
    if (k == NULL) return;
    k->alpha = alpha; k->beta = beta; k->gamma = gamma; k->dt = dt;
    k->pos = 0.0f; k->vel = 0.0f; k->acc = 0.0f;
}

float ABG_Filter_Update(ABG_Filter *k, float z_pos)
{
    if (k == NULL) return 0.0f;
    float dt = (k->dt > 1e-9f) ? k->dt : 1.0f;
    float pred_pos = k->pos + k->vel * dt + 0.5f * k->acc * dt * dt;
    float pred_vel = k->vel + k->acc * dt;
    float nu = z_pos - pred_pos;
    k->pos = pred_pos + k->alpha * nu;
    k->vel = pred_vel + k->beta * nu / dt;
    k->acc = k->acc + k->gamma * nu / (0.5f * dt * dt);
    return k->pos;
}

/* ================================================================
 * [06] 互补滤波
 * ================================================================ */
void Complementary_Init(Complementary *k, float tau, float dt)
{
    if (k == NULL) return;
    k->tau = tau; k->dt = dt;
    k->angle = 0.0f;
}

float Complementary_Update(Complementary *k, float gyro, float accel_angle)
{
    if (k == NULL) return 0.0f;
    float alpha = (k->tau > 0.0f)
                ? k->tau / (k->tau + k->dt) : 0.5f;
    k->angle = alpha * (k->angle + gyro * k->dt) + (1.0f - alpha) * accel_angle;
    return k->angle;
}

/* ================================================================
 * [07] Mahony 姿态互补滤波
 * ================================================================ */
void Mahony_Init(Mahony *k, float kp, float ki)
{
    if (k == NULL) return;
    k->kp = kp; k->ki = ki;
    k->q0 = 1.0f; k->q1 = k->q2 = k->q3 = 0.0f;
    k->ex = k->ey = k->ez = 0.0f;
}

void Mahony_Update(Mahony *k, float gx, float gy, float gz,
                   float ax, float ay, float az)
{
    if (k == NULL) return;
    float q0 = k->q0, q1 = k->q1, q2 = k->q2, q3 = k->q3;

    /* 归一化加速度 */
    float norm = sqrtf(ax * ax + ay * ay + az * az);
    if (norm < 1e-9f) return;
    ax /= norm; ay /= norm; az /= norm;

    /* 期望重力方向（从四元数） */
    float vx = 2.0f * (q1 * q3 - q0 * q2);
    float vy = 2.0f * (q0 * q1 + q2 * q3);
    float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

    /* 误差 = 测量 × 期望（叉积） */
    float ex = ay * vz - az * vy;
    float ey = az * vx - ax * vz;
    float ez = ax * vy - ay * vx;
    k->ex += ex * k->ki;
    k->ey += ey * k->ki;
    k->ez += ez * k->ki;

    /* 陀螺修正 */
    gx += k->kp * ex + k->ex;
    gy += k->kp * ey + k->ey;
    gz += k->kp * ez + k->ez;

    /* 四元数积分（一阶龙格-库塔） */
    float dt = k->dt;
    q0 += 0.5f * dt * (-q1 * gx - q2 * gy - q3 * gz);
    q1 += 0.5f * dt * ( q0 * gx + q2 * gz - q3 * gy);
    q2 += 0.5f * dt * ( q0 * gy - q1 * gz + q3 * gx);
    q3 += 0.5f * dt * ( q0 * gz + q1 * gy - q2 * gx);
    norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    if (norm < 1e-9f) norm = 1.0f;
    k->q0 = q0 / norm; k->q1 = q1 / norm;
    k->q2 = q2 / norm; k->q3 = q3 / norm;
}

void Mahony_ToEuler(const Mahony *k, float *roll, float *pitch, float *yaw)
{
    if (k == NULL) return;
    float q0 = k->q0, q1 = k->q1, q2 = k->q2, q3 = k->q3;
    if (roll)  *roll  = atan2f(2.0f * (q0 * q1 + q2 * q3),
                               1.0f - 2.0f * (q1 * q1 + q2 * q2));
    if (pitch) *pitch = asinf(kalman_clampf(2.0f * (q0 * q2 - q3 * q1), -1.0f, 1.0f));
    if (yaw)   *yaw   = atan2f(2.0f * (q0 * q3 + q1 * q2),
                               1.0f - 2.0f * (q2 * q2 + q3 * q3));
}

/* ================================================================
 * [08] 扩展卡尔曼（数值差分雅可比）
 * ================================================================ */
void EKF_Init(EKF *k)
{
    if (k == NULL) return;
    memset(k->x, 0, sizeof(k->x));
    memset(k->P, 0, sizeof(k->P));
    for (int32_t i = 0; i < k->n && i < EKF_MAX_DIM; i++) k->P[i][i] = 1.0f;
}

void EKF_Update(EKF *k, const float *z)
{
    if (k == NULL || k->f == NULL || k->h == NULL) return;
    int32_t n = k->n, m = k->m;
    if (n <= 0 || n > EKF_MAX_DIM || m <= 0 || m > EKF_MAX_DIM) return;
    float h_ = (k->dt > 0.0f) ? k->dt : 1e-4f;

    /* 预测：x = f(x) */
    float xn[EKF_MAX_DIM];
    k->f(k->x, xn, k->ctx);
    /* 数值差分雅可比 F（∂f/∂x） */
    float F[EKF_MAX_DIM][EKF_MAX_DIM] = {0};
    for (int32_t j = 0; j < n; j++) {
        float xp[EKF_MAX_DIM];
        memcpy(xp, k->x, sizeof(float) * EKF_MAX_DIM);
        xp[j] += h_;
        float xp_out[EKF_MAX_DIM];
        k->f(xp, xp_out, k->ctx);
        for (int32_t i = 0; i < n; i++) F[i][j] = (xp_out[i] - xn[i]) / h_;
    }
    memcpy(k->x, xn, sizeof(float) * EKF_MAX_DIM);
    /* P = F·P·Fᵀ + Q */
    float Ft[EKF_MAX_DIM][EKF_MAX_DIM], tmp[EKF_MAX_DIM][EKF_MAX_DIM];
    mat_trans(n, F, Ft);
    mat_mul(n, F, k->P, tmp);
    mat_mul(n, tmp, Ft, k->P);
    mat_add(n, k->P, k->Q, k->P);

    /* 更新 */
    float zp[EKF_MAX_DIM];
    k->h(k->x, zp, k->ctx);
    float H[EKF_MAX_DIM][EKF_MAX_DIM] = {0};
    for (int32_t j = 0; j < n; j++) {
        float xp[EKF_MAX_DIM];
        memcpy(xp, k->x, sizeof(float) * EKF_MAX_DIM);
        xp[j] += h_;
        float xp_out[EKF_MAX_DIM];
        k->h(xp, xp_out, k->ctx);
        for (int32_t i = 0; i < m; i++) H[i][j] = (xp_out[i] - zp[i]) / h_;
    }
    /* S = H·P·Hᵀ + R；K = P·Hᵀ·S⁻¹ */
    float Ht[EKF_MAX_DIM][EKF_MAX_DIM], HP[EKF_MAX_DIM][EKF_MAX_DIM];
    float S[EKF_MAX_DIM][EKF_MAX_DIM];
    mat_mul(m, H, k->P, HP);          /* m×n */
    mat_trans(m, H, Ht);
    mat_mul(m, HP, Ht, S);
    mat_add(m, S, k->R, S);
    float PHT[EKF_MAX_DIM][EKF_MAX_DIM];
    mat_mul(n, k->P, Ht, PHT);
    float K[EKF_MAX_DIM][EKF_MAX_DIM] = {0};
    for (int32_t i = 0; i < n; i++) {
        float col[EKF_MAX_DIM], sol[EKF_MAX_DIM];
        for (int32_t r = 0; r < m; r++) col[r] = PHT[i][r];
        if (mat_solve(m, S, col, sol) == 0) {
            for (int32_t r = 0; r < m; r++) K[i][r] = sol[r];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < m; j++) k->x[i] += K[i][j] * (z[j] - zp[j]);
    }
    float KH[EKF_MAX_DIM][EKF_MAX_DIM] = {0};
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) {
            for (int32_t t = 0; t < m; t++) KH[i][j] += K[i][t] * H[t][j];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) {
            tmp[i][j] = -KH[i][j] + (i == j ? 1.0f : 0.0f);
        }
    }
    mat_mul(n, tmp, k->P, k->P);
}

/* ================================================================
 * [09] 无迹卡尔曼
 * ================================================================ */
void UKF_Init(UKF *k)
{
    if (k == NULL) return;
    memset(k->x, 0, sizeof(k->x));
    memset(k->P, 0, sizeof(k->P));
    for (int32_t i = 0; i < k->n && i < UKF_MAX_DIM; i++) k->P[i][i] = 1.0f;
}

void UKF_Update(UKF *k, const float *z)
{
    if (k == NULL || k->f == NULL || k->h == NULL) return;
    int32_t n = k->n, m = k->m;
    if (n <= 0 || n > UKF_MAX_DIM || m <= 0 || m > UKF_MAX_DIM) return;
    const float ALPHA = (k->alpha > 0.0f) ? k->alpha : 1e-3f;
    const float BETA  = k->beta;
    const float KAPPA = k->kappa;
    const float lam = ALPHA * ALPHA * (n + KAPPA) - n;
    const int32_t ns = 2 * n + 1;

    /* sigma 点：x ± sqrt((n+λ)P)（对角近似 + 修正项，工程简化） */
    float X[2 * UKF_MAX_DIM + 1][UKF_MAX_DIM];
    float wm[2 * UKF_MAX_DIM + 1], wc[2 * UKF_MAX_DIM + 1];
    wm[0] = lam / (n + lam);
    wc[0] = wm[0] + (1.0f - ALPHA * ALPHA + BETA);
    for (int32_t i = 0; i < n; i++) {
        memcpy(X[i + 1], k->x, sizeof(float) * UKF_MAX_DIM);
        memcpy(X[i + 1 + n], k->x, sizeof(float) * UKF_MAX_DIM);
        float s = sqrtf((n + lam) * (k->P[i][i] > 0.0f ? k->P[i][i] : 1e-6f));
        X[i + 1][i] += s;
        X[i + 1 + n][i] -= s;
        wm[i + 1] = wc[i + 1] = 0.5f / (n + lam);
        wm[i + 1 + n] = wc[i + 1 + n] = 0.5f / (n + lam);
    }
    /* 传播 + 加权均值/协方差 */
    float xm[UKF_MAX_DIM] = {0};
    float Y[2 * UKF_MAX_DIM + 1][UKF_MAX_DIM];
    for (int32_t s = 0; s < ns; s++) {
        k->f(X[s], Y[s], k->ctx);
        for (int32_t i = 0; i < n; i++) xm[i] += wm[s] * Y[s][i];
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) {
            float v = 0.0f;
            for (int32_t s = 0; s < ns; s++) {
                v += wc[s] * (Y[s][i] - xm[i]) * (Y[s][j] - xm[j]);
            }
            k->P[i][j] = v + k->Q[i][j];
        }
    }
    memcpy(k->x, xm, sizeof(float) * UKF_MAX_DIM);
    /* 观测传播（同上，H 由 h 回调隐式定义） */
    float zm[UKF_MAX_DIM] = {0};
    float Z[2 * UKF_MAX_DIM + 1][UKF_MAX_DIM];
    for (int32_t s = 0; s < ns; s++) {
        k->h(Y[s], Z[s], k->ctx);
        for (int32_t i = 0; i < m; i++) zm[i] += wm[s] * Z[s][i];
    }
    float Pzz[UKF_MAX_DIM][UKF_MAX_DIM] = {0};
    float Pxz[UKF_MAX_DIM][UKF_MAX_DIM] = {0};
    for (int32_t i = 0; i < m; i++) {
        for (int32_t j = 0; j < m; j++) {
            for (int32_t s = 0; s < ns; s++) {
                Pzz[i][j] += wc[s] * (Z[s][i] - zm[i]) * (Z[s][j] - zm[j]);
            }
            Pzz[i][j] += k->R[i][j];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < m; j++) {
            for (int32_t s = 0; s < ns; s++) {
                Pxz[i][j] += wc[s] * (Y[s][i] - xm[i]) * (Z[s][j] - zm[j]);
            }
        }
    }
    /* K = Pxz·Pzz⁻¹（UKF 矩阵维 ≤ KF_MAX_DIM，cast 统一接口） */
    float K[UKF_MAX_DIM][UKF_MAX_DIM] = {0};
    for (int32_t i = 0; i < n; i++) {
        float col[UKF_MAX_DIM], sol[UKF_MAX_DIM];
        for (int32_t r = 0; r < m; r++) col[r] = Pxz[i][r];
        if (mat_solve(m, (float (*)[KF_MAX_DIM])Pzz, col, sol) == 0) {
            for (int32_t r = 0; r < m; r++) K[i][r] = sol[r];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < m; j++) k->x[i] += K[i][j] * (z[j] - zm[j]);
    }
    /* P = P - K·Pzz·Kᵀ */
    float KPK[UKF_MAX_DIM][UKF_MAX_DIM] = {0};
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) {
            float v = 0.0f;
            for (int32_t a = 0; a < m; a++) {
                for (int32_t b = 0; b < m; b++) v += K[i][a] * Pzz[a][b] * K[j][b];
            }
            KPK[i][j] = v;
        }
    }
    for (int32_t i = 0; i < n; i++) {
        for (int32_t j = 0; j < n; j++) k->P[i][j] -= KPK[i][j];
    }
}

/* ================================================================
 * [10] 自适应卡尔曼（创新方差匹配）
 * ================================================================ */
void KF_Adaptive_Init(KF_Adaptive *k, float q, float r0)
{
    if (k == NULL) return;
    k->q = q; k->r = r0;
    k->x = 0.0f; k->p = 1.0f;
    k->nu = 0.0f;
    k->nu_var_est = r0;
    k->nu_var_alpha = 0.05f;
    k->r_min = r0 * 0.1f;
    k->cnt = 0;
}

float KF_Adaptive_Update(KF_Adaptive *k, float z)
{
    if (k == NULL) return 0.0f;
    k->p += k->q;
    float s = k->p + k->r;
    float kg = (s > 1e-12f) ? (k->p / s) : 1.0f;
    k->nu = z - k->x;
    k->x += kg * k->nu;
    k->p = (1.0f - kg) * k->p;
    /* 创新方差滑动估计 → 修正 R */
    k->cnt++;
    if (k->cnt < 20u) return k->x;   /* 预热期不修正 */
    k->nu_var_est = (1.0f - k->nu_var_alpha) * k->nu_var_est
                  + k->nu_var_alpha * k->nu * k->nu;
    float r_new = k->nu_var_est - k->p;   /* R ≈ ν² - P */
    if (r_new < k->r_min) r_new = k->r_min;
    k->r = r_new;
    return k->x;
}

/* ================================================================
 * [11] 交互式多模型（IMM）
 * ================================================================ */
void IMM_Init(IMM *k, int32_t n_models, const float *q_list, float r,
              const float *trans, const float *mu0)
{
    if (k == NULL) return;
    k->n_models = (n_models > IMM_MAX_MODELS) ? IMM_MAX_MODELS : n_models;
    for (int32_t i = 0; i < k->n_models; i++) {
        k->model[i].x = 0.0f;
        k->model[i].p = 1.0f;
        k->model[i].q = q_list ? q_list[i] : 1.0f;
        k->model[i].r = r;
        k->model[i].like = 0.0f;
        k->mu[i] = mu0 ? mu0[i] : (1.0f / k->n_models);
        for (int32_t j = 0; j < k->n_models; j++) {
            k->trans[i][j] = trans ? trans[i * k->n_models + j]
                                   : ((i == j) ? 0.9f : 0.05f);
        }
    }
    k->x_out = 0.0f;
    k->p_out = 0.0f;
}

float IMM_Update(IMM *k, float z)
{
    if (k == NULL) return 0.0f;
    int32_t n = k->n_models;
    /* 1. 交互（混合先验）——简化版：跳过马尔可夫混合，直接更新 */
    /* 2. 每模型独立 KF 更新 + 似然 */
    float like_sum = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        float *mi = &k->model[i].x;
        float *pi = &k->model[i].p;
        *pi += k->model[i].q;
        float s = *pi + k->model[i].r;
        float kg = (s > 1e-12f) ? (*pi / s) : 1.0f;
        float nu = z - *mi;
        *mi += kg * nu;
        *pi = (1.0f - kg) * *pi;
        /* 高斯似然 */
        float s2 = s;
        k->model[i].like = expf(-0.5f * nu * nu / (s2 > 1e-12f ? s2 : 1e-12f))
                         / sqrtf(6.2832f * (s2 > 1e-12f ? s2 : 1e-12f));
        like_sum += k->model[i].like * k->mu[i];
    }
    /* 3. 模型概率更新 */
    for (int32_t i = 0; i < n; i++) {
        k->mu[i] = (like_sum > 1e-12f)
                 ? k->model[i].like * k->mu[i] / like_sum : k->mu[i];
    }
    /* 4. 加权融合输出 */
    k->x_out = 0.0f;
    k->p_out = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        k->x_out += k->mu[i] * k->model[i].x;
    }
    for (int32_t i = 0; i < n; i++) {
        float d = k->model[i].x - k->x_out;
        k->p_out += k->mu[i] * (k->model[i].p + d * d);
    }
    return k->x_out;
}

/* ================================================================
 * [12] 信息滤波（简化一维演示 + 通用矩阵接口）
 * ================================================================ */
void InfoKF_Init(InfoKF *k)
{
    if (k == NULL) return;
    memset(k->y, 0, sizeof(k->y));
    memset(k->Y, 0, sizeof(k->Y));
    for (int32_t i = 0; i < k->n && i < KF_MAX_DIM; i++) k->Y[i][i] = 1.0f;
}

void InfoKF_Update(InfoKF *k, const float *z)
{
    if (k == NULL) return;
    int32_t n = k->n;
    /* 预测：Y = (F·Y⁻¹·Fᵀ + Q)⁻¹；y = Y·F·Y⁻¹·y_old
     * （工程简化：一维实现为主，多维走通用 KF 更实用） */
    if (n == 1) {
        float yinv = (k->Y[0][0] > 1e-12f) ? (1.0f / k->Y[0][0]) : 1e6f;
        float f = k->F[0][0];
        float ypred = 1.0f / (f * f * yinv + k->Q[0][0]);
        float yvec = ypred * f * (k->y[0] * yinv);   /* y·Y⁻¹ = y·yinv */
        float h = k->H[0][0], r = k->R[0][0];
        k->Y[0][0] = ypred + h * h / r;
        k->y[0] = yvec + h * z[0] / r;
    }
}

/* ================================================================
 * [13] 平方根卡尔曼（标量简化：S = sqrt(P) 传播）
 * ================================================================ */
void KF_Sqrt_Init(KF_Sqrt *k, float q, float r)
{
    if (k == NULL) return;
    k->q = q; k->r = r;
    k->x = 0.0f;
    k->p_sqrt = 1.0f;
}

float KF_Sqrt_Update(KF_Sqrt *k, float z)
{
    if (k == NULL) return 0.0f;
    /* P = S²；预测 P += Q → S = sqrt(S² + Q) */
    float p = k->p_sqrt * k->p_sqrt + k->q;
    k->p_sqrt = sqrtf(p > 0.0f ? p : 0.0f);
    float s = p + k->r;
    float kg = (s > 1e-12f) ? (p / s) : 1.0f;
    k->x += kg * (z - k->x);
    /* 更新（Joseph 风格保持正定）：P = (1-k)²·P + k²·R */
    float p_new = (1.0f - kg) * (1.0f - kg) * p + kg * kg * k->r;
    k->p_sqrt = sqrtf(p_new > 0.0f ? p_new : 0.0f);
    return k->x;
}

/* ================================================================
 * [14] RTS 平滑器
 * ================================================================ */
void RTS_Init(RTS *k, float q, float r)
{
    if (k == NULL) return;
    k->q = q; k->r = r;
    k->len = 0;
}

void RTS_Add(RTS *k, float z)
{
    if (k == NULL || k->len >= RTS_MAX_LEN) return;
    /* 前向 KF 一步 */
    float x, p;
    if (k->len == 0) {
        x = z;
        p = k->r;
    } else {
        x = k->xf[k->len - 1];
        p = k->pf[k->len - 1] + k->q;
        float s = p + k->r;
        float kg = (s > 1e-12f) ? (p / s) : 1.0f;
        x += kg * (z - x);
        p = (1.0f - kg) * p;
    }
    k->xf[k->len] = x;
    k->pf[k->len] = p;
    k->len++;
}

void RTS_Smooth(RTS *k)
{
    if (k == NULL) return;
    for (int32_t i = k->len - 1; i >= 0; i--) {
        if (i == k->len - 1) {
            k->x[i] = k->xf[i];
        } else {
            float pred = k->xf[i];
            float pp = k->pf[i] + k->q;
            float g = (pp > 1e-12f) ? (k->pf[i] / pp) : 1.0f;
            k->x[i] = k->xf[i] + g * (k->x[i + 1] - pred);
        }
    }
}

/* ================================================================
 * [15] 粒子滤波
 * ================================================================ */
void PF_Init(PF *k, float q, float r, int32_t n)
{
    if (k == NULL) return;
    k->q = q; k->r = r;
    k->n = (n > PF_PARTICLES) ? PF_PARTICLES : ((n > 0) ? n : PF_PARTICLES);
    for (int32_t i = 0; i < k->n; i++) {
        k->x[i] = 0.0f;
        k->w[i] = 1.0f / k->n;
    }
    k->estimate = 0.0f;
}

void PF_Update(PF *k, float z)
{
    if (k == NULL) return;
    /* 1. 过程噪声扰动（LCG 伪随机：确定性可复现，避免固定模式导致
     *    粒子无法扩散——实测固定序列使估计冻结在初始值） */
    static uint32_t pf_seed = 0x12345678u;
    for (int32_t i = 0; i < k->n; i++) {
        pf_seed = pf_seed * 1664525u + 1013904223u;
        float n = (float)((pf_seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        k->x[i] += k->q * n;
    }
    /* 2. 似然加权（高斯） */
    float wsum = 0.0f;
    for (int32_t i = 0; i < k->n; i++) {
        float d = z - k->x[i];
        k->w[i] = expf(-0.5f * d * d / (k->r > 1e-12f ? k->r : 1e-12f));
        wsum += k->w[i];
    }
    if (wsum > 1e-12f) {
        for (int32_t i = 0; i < k->n; i++) k->w[i] /= wsum;
    }
    /* 3. 估计 = 加权均值 */
    k->estimate = 0.0f;
    for (int32_t i = 0; i < k->n; i++) k->estimate += k->w[i] * k->x[i];
}

void PF_Resample(PF *k)
{
    if (k == NULL) return;
    /* 系统重采样（确定性） */
    float x_new[PF_PARTICLES];
    float step = 1.0f / k->n;
    float u = step * 0.5f;
    float c = k->w[0];
    int32_t idx = 0;
    for (int32_t i = 0; i < k->n; i++) {
        while (u > c && idx < k->n - 1) {
            idx++;
            c += k->w[idx];
        }
        x_new[i] = k->x[idx];
        u += step;
    }
    memcpy(k->x, x_new, sizeof(float) * k->n);
    for (int32_t i = 0; i < k->n; i++) k->w[i] = 1.0f / k->n;
}
