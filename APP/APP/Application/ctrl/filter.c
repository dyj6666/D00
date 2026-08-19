/* ================================================================
 * filter.c —— 通用信号滤波全家族实现（14 种）
 * ================================================================ */
#include "filter.h"
#include <math.h>
#include <string.h>

/* ================================================================
 * [01] 一阶低通
 * ================================================================ */
void LPF_1st_Init(LPF_1st *f, float alpha, float y0)
{
    if (f == NULL) return;
    f->alpha = alpha;
    f->y = y0;
}

float LPF_1st_Update(LPF_1st *f, float x)
{
    if (f == NULL) return 0.0f;
    f->y += f->alpha * (x - f->y);
    return f->y;
}

/* ================================================================
 * [02] 一阶高通
 * ================================================================ */
void HPF_1st_Init(HPF_1st *f, float alpha)
{
    if (f == NULL) return;
    f->alpha = alpha;
    f->y = 0.0f;
    f->x_prev = 0.0f;
}

float HPF_1st_Update(HPF_1st *f, float x)
{
    if (f == NULL) return 0.0f;
    f->y = f->alpha * (f->y + x - f->x_prev);
    f->x_prev = x;
    return f->y;
}

/* ================================================================
 * [03] 指数移动平均
 * ================================================================ */
void EMA_Init(EMA *f, float alpha)
{
    if (f == NULL) return;
    f->alpha = alpha;
    f->y = 0.0f;
}

float EMA_Update(EMA *f, float x)
{
    if (f == NULL) return 0.0f;
    f->y += f->alpha * (x - f->y);
    return f->y;
}

/* ================================================================
 * [04] 滑动平均
 * ================================================================ */
void MovingAverage_Init(MovingAverage *f, uint32_t len)
{
    if (f == NULL) return;
    f->len = (len > MAV_MAX_WIN) ? MAV_MAX_WIN : ((len > 0) ? len : 1);
    memset(f->buf, 0, sizeof(f->buf));
    f->idx = 0;
    f->sum = 0.0f;
    f->y = 0.0f;
}

float MovingAverage_Update(MovingAverage *f, float x)
{
    if (f == NULL) return 0.0f;
    f->sum -= f->buf[f->idx];
    f->buf[f->idx] = x;
    f->sum += x;
    f->idx = (f->idx + 1u) % f->len;
    f->y = f->sum / (float)f->len;
    return f->y;
}

/* ================================================================
 * [05] 中值滤波（插入排序，窗口小）
 * ================================================================ */
void Median_Init(Median *f, uint32_t len)
{
    if (f == NULL) return;
    f->len = (len > MED_MAX_WIN) ? MED_MAX_WIN : ((len > 0) ? len : 1);
    memset(f->buf, 0, sizeof(f->buf));
    f->idx = 0;
    f->y = 0.0f;
}

float Median_Update(Median *f, float x)
{
    if (f == NULL) return 0.0f;
    f->buf[f->idx] = x;
    f->idx = (f->idx + 1u) % f->len;
    /* 拷贝排序取中值（窗口小，插入排序足够） */
    float tmp[MED_MAX_WIN];
    memcpy(tmp, f->buf, sizeof(float) * f->len);
    for (uint32_t i = 1; i < f->len; i++) {
        float v = tmp[i];
        uint32_t j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    f->y = tmp[f->len / 2u];
    return f->y;
}

/* ================================================================
 * [06] 限幅滤波
 * ================================================================ */
void Limit_Init(Limit *f, float limit, float y0)
{
    if (f == NULL) return;
    f->limit = limit;
    f->y = y0;
}

float Limit_Update(Limit *f, float x)
{
    if (f == NULL) return 0.0f;
    float d = x - f->y;
    if (d > f->limit) d = f->limit;
    else if (d < -f->limit) d = -f->limit;
    f->y += d;
    return f->y;
}

/* ================================================================
 * [07] 消抖滤波
 * ================================================================ */
void Debounce_Init(Debounce *f, uint32_t need, uint8_t init_state)
{
    if (f == NULL) return;
    f->need = (need > 0) ? need : 1;
    f->cnt = 0;
    f->state = init_state;
    f->last = init_state;
}

uint8_t Debounce_Update(Debounce *f, uint8_t x)
{
    if (f == NULL) return 0;
    if (x == f->last) {
        f->cnt++;
        if (f->cnt >= f->need) {
            f->state = x;
        }
    } else {
        f->last = x;
        f->cnt = 0;
    }
    return f->state;
}

/* ================================================================
 * [08] 陷波滤波（RBJ 带阻系数）
 * ================================================================ */
void Notch_Init(Notch *f, float fs, float f0, float q)
{
    if (f == NULL || fs <= 0.0f) return;
    float w0 = 6.2832f * f0 / fs;
    float cw = cosf(w0);
    float alpha = (q > 1e-6f) ? (sinf(w0) / (2.0f * q)) : 0.0f;
    float a0 = 1.0f + alpha;
    f->b0 = 1.0f / a0;
    f->b1 = -2.0f * cw / a0;
    f->b2 = 1.0f / a0;
    f->a1 = -2.0f * cw / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
    f->y = 0.0f;
}

float Notch_Update(Notch *f, float x)
{
    if (f == NULL) return 0.0f;
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
            - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    f->y = y;
    return y;
}

/* ================================================================
 * [09] 双二阶 IIR（RBJ 公式）
 * ================================================================ */
void Biquad_Init(Biquad *f, Biquad_Type type, float fs, float fc, float q)
{
    if (f == NULL || fs <= 0.0f) return;
    float w0 = 6.2832f * fc / fs;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = (q > 1e-6f) ? (sw / (2.0f * q)) : 0.0f;
    float a0 = 1.0f + alpha;
    switch (type) {
    case BIQUAD_LPF:
        f->b0 = (1.0f - cw) / 2.0f / a0;
        f->b1 = (1.0f - cw) / a0;
        f->b2 = f->b0;
        break;
    case BIQUAD_HPF:
        f->b0 = (1.0f + cw) / 2.0f / a0;
        f->b1 = -(1.0f + cw) / a0;
        f->b2 = f->b0;
        break;
    case BIQUAD_BPF:
        f->b0 = alpha / a0;
        f->b1 = 0.0f;
        f->b2 = -alpha / a0;
        break;
    default: /* BSF */
        f->b0 = 1.0f / a0;
        f->b1 = -2.0f * cw / a0;
        f->b2 = 1.0f / a0;
        break;
    }
    f->a1 = -2.0f * cw / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
    f->y = 0.0f;
}

float Biquad_Update(Biquad *f, float x)
{
    if (f == NULL) return 0.0f;
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
            - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    f->y = y;
    return y;
}

/* ================================================================
 * [10] 巴特沃斯低通（级联）
 * ================================================================ */
void Butterworth_Init(Butterworth *f, uint32_t order, float fs, float fc)
{
    if (f == NULL || order == 0) return;
    /* 阶数 → 双二阶节数（奇偶统一：ceil(order/2)） */
    uint32_t n_sec = (order + 1u) / 2u;
    if (n_sec > BUTTER_MAX_SECTIONS) n_sec = BUTTER_MAX_SECTIONS;
    f->n_sec = n_sec;
    float wc = 6.2832f * fc / fs;
    for (uint32_t i = 0; i < n_sec; i++) {
        /* 巴特沃斯极点对：θ = π(2i+1)/(2n)（预畸变近似） */
        float theta = 3.14159f * (2.0f * (float)i + 1.0f) / (2.0f * (float)order);
        float q = 1.0f / (2.0f * cosf(theta));
        if (order & 1u) {           /* 奇阶：首节为一阶 */
            if (i == 0) {
                /* 一阶节：H(s) = 1/(1 + s/ωc) */
                float t = tanf(wc / 2.0f);
                float a0 = 1.0f + t;
                f->sec[0].b0 = t / a0;
                f->sec[0].b1 = t / a0;
                f->sec[0].b2 = 0.0f;
                f->sec[0].a1 = (t - 1.0f) / a0;
                f->sec[0].a2 = 0.0f;
                f->sec[0].x1 = f->sec[0].x2 = 0.0f;
                f->sec[0].y1 = f->sec[0].y2 = 0.0f;
                continue;
            }
            theta = 3.14159f * (2.0f * (float)(i) + 1.0f) / (2.0f * (float)order);
            q = 1.0f / (2.0f * cosf(theta));
        }
        /* 二阶节：双线性变换 */
        float t = tanf(wc / 2.0f);
        float t2 = t * t;
        float a0 = t2 + t / q + 1.0f;
        Biquad *s = &f->sec[i];
        s->b0 = t2 / a0;
        s->b1 = 2.0f * t2 / a0;
        s->b2 = t2 / a0;
        s->a1 = 2.0f * (t2 - 1.0f) / a0;
        s->a2 = (t2 - t / q + 1.0f) / a0;
        s->x1 = s->x2 = s->y1 = s->y2 = 0.0f;
    }
}

float Butterworth_Update(Butterworth *f, float x)
{
    if (f == NULL) return 0.0f;
    float v = x;
    for (uint32_t i = 0; i < f->n_sec; i++) {
        v = Biquad_Update(&f->sec[i], v);
    }
    return v;
}

/* ================================================================
 * [11] Savitzky-Golay 平滑（5 点 2 次多项式）
 *  卷积核：[−3, 12, 17, 12, −3] / 35
 * ================================================================ */
void SavitzkyGolay_Init(SavitzkyGolay *f)
{
    if (f == NULL) return;
    memset(f->buf, 0, sizeof(f->buf));
    f->idx = 0;
    f->filled = 0;
    f->y = 0.0f;
}

float SavitzkyGolay_Update(SavitzkyGolay *f, float x)
{
    if (f == NULL) return 0.0f;
    f->buf[f->idx] = x;
    f->idx = (f->idx + 1u) % SG_WIN;
    if (f->filled < SG_WIN) f->filled++;
    if (f->filled < SG_WIN) return x;
    /* 环形取序（最旧→最新） */
    float s[SG_WIN];
    for (uint32_t i = 0; i < SG_WIN; i++) {
        s[i] = f->buf[(f->idx + i) % SG_WIN];
    }
    /* 卷积核 [-3, 12, 17, 12, -3]/35：中心平滑 */
    f->y = (-3.0f * s[0] + 12.0f * s[1] + 17.0f * s[2]
            + 12.0f * s[3] - 3.0f * s[4]) / 35.0f;
    return f->y;
}

/* ================================================================
 * [12] 加权平均
 * ================================================================ */
void WeightedAvg_Init(WeightedAvg *f, uint32_t n)
{
    if (f == NULL) return;
    f->n = (n > WAVG_MAX_SRC) ? WAVG_MAX_SRC : n;
    for (uint32_t i = 0; i < WAVG_MAX_SRC; i++) {
        f->x[i] = 0.0f;
        f->w[i] = 1.0f;
    }
    f->y = 0.0f;
}

void WeightedAvg_Set(WeightedAvg *f, uint32_t i, float x, float w)
{
    if (f == NULL || i >= WAVG_MAX_SRC) return;
    f->x[i] = x;
    f->w[i] = (w >= 0.0f) ? w : 0.0f;
}

float WeightedAvg_Calc(WeightedAvg *f)
{
    if (f == NULL) return 0.0f;
    float num = 0.0f, den = 0.0f;
    for (uint32_t i = 0; i < f->n; i++) {
        num += f->x[i] * f->w[i];
        den += f->w[i];
    }
    f->y = (den > 1e-12f) ? (num / den) : 0.0f;
    return f->y;
}

/* ================================================================
 * [13] 死区滤波
 * ================================================================ */
void Deadband_Init(Deadband *f, float deadband)
{
    if (f == NULL) return;
    f->deadband = deadband;
    f->y = 0.0f;
}

float Deadband_Update(Deadband *f, float x)
{
    if (f == NULL) return 0.0f;
    if (x > f->deadband) f->y = x - f->deadband;
    else if (x < -f->deadband) f->y = x + f->deadband;
    else f->y = 0.0f;
    return f->y;
}

/* ================================================================
 * [14] 变化率限制
 * ================================================================ */
void RateLimiter_Init(RateLimiter *f, float rate, float dt, float y0)
{
    if (f == NULL) return;
    f->rate = rate;
    f->dt = dt;
    f->y = y0;
}

float RateLimiter_Update(RateLimiter *f, float x)
{
    if (f == NULL) return 0.0f;
    float max_step = f->rate * f->dt;
    float d = x - f->y;
    if (d > max_step) d = max_step;
    else if (d < -max_step) d = -max_step;
    f->y += d;
    return f->y;
}
