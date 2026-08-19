/* ================================================================
 * filter.h —— 通用信号滤波全家族（14 种）
 *
 * 架构位置：APP 应用层 ctrl/ 子模块；纯 C、float、结构体实例化。
 *
 * 家族索引（按用途选型）：
 *   [01] LPF_1st       一阶低通（RC 惯性，最常用）
 *   [02] HPF_1st       一阶高通（去直流/趋势）
 *   [03] EMA           指数移动平均（轻量平滑，α 可调）
 *   [04] MovingAverage 滑动平均（N 点窗，相位线性）
 *   [05] Median        中值滤波（脉冲/椒盐噪声克星）
 *   [06] Limit         限幅滤波（相邻采样差限幅，防突变）
 *   [07] Debounce      消抖滤波（数字量：连续 N 次确认）
 *   [08] Notch         陷波滤波（50Hz 工频/特定频率）
 *   [09] Biquad        双二阶 IIR（低/高/带通/带阻，系数生成器）
 *   [10] Butterworth   巴特沃斯低通（级联双二阶，任意阶）
 *   [11] SavitzkyGolay SG 平滑（多项式拟合，保形）
 *   [12] WeightedAvg   加权平均（信任度融合）
 *   [13] Deadband      死区滤波（小信号置零）
 *   [14] RateLimiter   变化率限制（斜坡，执行器保护）
 *
 * 使用场景索引：
 *   传感器原始数据 → 01/03/04；脉冲干扰 → 05/06；
 *   数字信号（按键）→ 07；工频干扰 → 08；精确频响 → 09/10；
 *   曲线保形 → 11；多传感器融合 → 12；机械执行器 → 13/14。
 * ================================================================ */
#ifndef FILTER_H
#define FILTER_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * [01] 一阶低通（RC）
 *  y += α·(x - y)，α = dt/(RC+dt) 或直接给 0~1。
 *  场景：传感器噪声平滑（加速度/温度/电压）。
 * ---------------------------------------------------------------- */
typedef struct {
    float alpha;            /* 滤波系数 0~1（越大越灵敏） */
    float y;                /* 输出（读） */
} LPF_1st;

void LPF_1st_Init(LPF_1st *f, float alpha, float y0);
float LPF_1st_Update(LPF_1st *f, float x);

/* ----------------------------------------------------------------
 * [02] 一阶高通
 *  y = α·(y + x - x_prev)——去直流/慢漂移。
 *  场景：去趋势、去重力偏置、信号微分预处理。
 * ---------------------------------------------------------------- */
typedef struct {
    float alpha;
    float y;
    float x_prev;
} HPF_1st;

void HPF_1st_Init(HPF_1st *f, float alpha);
float HPF_1st_Update(HPF_1st *f, float x);

/* ----------------------------------------------------------------
 * [03] 指数移动平均
 *  y += α·(x - y)。与 LPF_1st 同构，独立提供便于参数直觉。
 * ---------------------------------------------------------------- */
typedef struct {
    float alpha;
    float y;
} EMA;

void EMA_Init(EMA *f, float alpha);
float EMA_Update(EMA *f, float x);

/* ----------------------------------------------------------------
 * [04] 滑动平均（N 点窗）
 *  环形缓冲，相位线性、无过冲；延迟 = (N-1)/2 采样。
 *  场景：需要波形形状保真的平滑。
 * ---------------------------------------------------------------- */
#define MAV_MAX_WIN   32

typedef struct {
    float buf[MAV_MAX_WIN];
    uint32_t len;           /* 窗口长度 */
    uint32_t idx;
    float sum;
    float y;
} MovingAverage;

void MovingAverage_Init(MovingAverage *f, uint32_t len);
float MovingAverage_Update(MovingAverage *f, float x);

/* ----------------------------------------------------------------
 * [05] 中值滤波
 *  滑动窗排序取中值——脉冲/椒盐噪声下输出几乎不受污染。
 *  场景：传感器偶发尖峰（电机换向、电源干扰）。
 * ---------------------------------------------------------------- */
#define MED_MAX_WIN   9

typedef struct {
    float buf[MED_MAX_WIN];
    uint32_t len;
    uint32_t idx;
    float y;
} Median;

void Median_Init(Median *f, uint32_t len);
float Median_Update(Median *f, float x);

/* ----------------------------------------------------------------
 * [06] 限幅滤波（防脉冲）
 *  |x - y_prev| > limit → 丢弃（保持），否则通过。
 *  场景：遥测跳变、粗差剔除。
 * ---------------------------------------------------------------- */
typedef struct {
    float limit;
    float y;
} Limit;

void Limit_Init(Limit *f, float limit, float y0);
float Limit_Update(Limit *f, float x);

/* ----------------------------------------------------------------
 * [07] 消抖滤波（数字量）
 *  连续 N 次采样一致才翻转输出——按键/开关/限位经典。
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t need;          /* 需要连续一致的次数 */
    uint32_t cnt;
    uint8_t  state;         /* 当前输出 */
    uint8_t  last;          /* 上次输入 */
} Debounce;

void Debounce_Init(Debounce *f, uint32_t need, uint8_t init_state);
uint8_t Debounce_Update(Debounce *f, uint8_t x);

/* ----------------------------------------------------------------
 * [08] 陷波滤波（IIR 双二阶带阻）
 *  指定中心频率（如 50Hz 工频）+ Q 值深度抑制。
 *  场景：电源 50Hz、谐振峰、特定机械振动。
 * ---------------------------------------------------------------- */
typedef struct {
    float b0, b1, b2;       /* 分子系数 */
    float a1, a2;           /* 分母系数（a0=1） */
    float x1, x2, y1, y2;
    float y;
} Notch;

/* fs：采样率；f0：陷波频率；Q：品质因数（越大带宽越窄） */
void Notch_Init(Notch *f, float fs, float f0, float q);
float Notch_Update(Notch *f, float x);

/* ----------------------------------------------------------------
 * [09] 双二阶 IIR（通用滤波器单元）
 *  低通/高通/带通/带阻四型系数生成（RBJ 公式）。
 *  场景：任意 IIR 设计的基础积木。
 * ---------------------------------------------------------------- */
typedef enum {
    BIQUAD_LPF = 0,
    BIQUAD_HPF,
    BIQUAD_BPF,
    BIQUAD_BSF,
} Biquad_Type;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2, y1, y2;
    float y;
} Biquad;

void Biquad_Init(Biquad *f, Biquad_Type type, float fs, float fc, float q);
float Biquad_Update(Biquad *f, float x);

/* ----------------------------------------------------------------
 * [10] 巴特沃斯低通（级联双二阶，任意阶）
 *  最平坦幅频响应；2~8 阶由级联数决定。
 *  场景：精密测量通道、抗混叠。
 * ---------------------------------------------------------------- */
#define BUTTER_MAX_SECTIONS   4   /* 8 阶 */

typedef struct {
    Biquad sec[BUTTER_MAX_SECTIONS];
    uint32_t n_sec;
} Butterworth;

void Butterworth_Init(Butterworth *f, uint32_t order, float fs, float fc);
float Butterworth_Update(Butterworth *f, float x);

/* ----------------------------------------------------------------
 * [11] Savitzky-Golay 平滑（5 点 2 次多项式）
 *  最小二乘多项式拟合——保形（峰/谷不衰减）优于滑动平均。
 *  场景：光谱/曲线保形平滑、导数估计。
 * ---------------------------------------------------------------- */
#define SG_WIN   5

typedef struct {
    float buf[SG_WIN];      /* 滑动窗 */
    uint32_t idx;
    uint32_t filled;
    float y;
} SavitzkyGolay;

void SavitzkyGolay_Init(SavitzkyGolay *f);
/* 连续喂入最新样本，内部 5 点窗输出中心平滑值 */
float SavitzkyGolay_Update(SavitzkyGolay *f, float x);

/* ----------------------------------------------------------------
 * [12] 加权平均（信任度融合）
 *  y = Σ(w_i·x_i)/Σw_i——多传感器按噪声方差加权。
 *  场景：双 IMU、GPS+里程计、视觉+惯性融合。
 * ---------------------------------------------------------------- */
#define WAVG_MAX_SRC   4

typedef struct {
    float x[WAVG_MAX_SRC];
    float w[WAVG_MAX_SRC];
    uint32_t n;
    float y;
} WeightedAvg;

void WeightedAvg_Init(WeightedAvg *f, uint32_t n);
void WeightedAvg_Set(WeightedAvg *f, uint32_t i, float x, float w);
float WeightedAvg_Calc(WeightedAvg *f);

/* ----------------------------------------------------------------
 * [13] 死区滤波
 *  |x| < deadband → 输出 0（小信号/静区抑制）。
 *  场景：传感器微小抖动抑制、零位校准。
 * ---------------------------------------------------------------- */
typedef struct {
    float deadband;
    float y;
} Deadband;

void Deadband_Init(Deadband *f, float deadband);
float Deadband_Update(Deadband *f, float x);

/* ----------------------------------------------------------------
 * [14] 变化率限制（斜坡）
 *  |Δy| ≤ rate·dt——输出平滑斜坡，执行器/指令保护。
 *  场景：舵机指令斜坡、功率斜坡、油门限制。
 * ---------------------------------------------------------------- */
typedef struct {
    float rate;             /* 变化率上限（单位/s） */
    float dt;
    float y;
} RateLimiter;

void RateLimiter_Init(RateLimiter *f, float rate, float dt, float y0);
float RateLimiter_Update(RateLimiter *f, float x);

#ifdef __cplusplus
}
#endif

#endif /* FILTER_H */
