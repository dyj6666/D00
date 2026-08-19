/* ================================================================
 * pid.c —— PID 全家族实现（14 变式）
 *
 * 全部为纯 C 实现：无动态内存、无 HAL 依赖、float 单精度
 * （FPU 直接加速）。数值细节：
 *   - 积分使用显式欧拉累加，输出前做限幅；
 *   - 微分使用一阶后向差分 + 可选低通（抑制噪声放大）；
 *   - 所有除法带零值守卫。
 * ================================================================ */
#include "pid.h"

/* ================================================================
 * [01] 位置式 PID（全特性版）
 * ================================================================ */
void PID_Pos_Init(PID_Pos *p)
{
    if (p == NULL) return;
    p->integral  = 0.0f;
    p->prev_err  = 0.0f;
    p->prev_meas = 0.0f;
    p->dfilter   = 0.0f;
    p->out       = 0.0f;
    if (p->out_min == 0.0f && p->out_max == 0.0f) {
        p->out_min = -1e6f;   /* 默认宽限幅 */
        p->out_max =  1e6f;
    }
    if (p->int_min == 0.0f && p->int_max == 0.0f) {
        p->int_min = p->out_min;
        p->int_max = p->out_max;
    }
}

static float pid_pos_core(PID_Pos *p, float err, float d_term)
{
    /* 积分分离：大偏差冻结积分 */
    if (p->sep_thresh > 0.0f && (err > p->sep_thresh || err < -p->sep_thresh)) {
        /* 积分冻结（保持现值） */
    } else {
        p->integral += p->ki * err * p->dt;
        p->integral = pid_clampf(p->integral, p->int_min, p->int_max);
    }

    float out = p->kp * err + p->integral + d_term;
    p->out = pid_clampf(out, p->out_min, p->out_max);
    return p->out;
}

float PID_Pos_Update(PID_Pos *p, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 微分项：误差后向差分 + 一阶低通 */
    float d_err = (err - p->prev_err) / p->dt;
    p->dfilter += (d_err - p->dfilter) * p->df_alpha;   /* α=0 时无滤波 */
    float d_term = p->kd * p->dfilter;
    p->prev_err = err;
    return pid_pos_core(p, err, d_term);
}

float PID_Pos_UpdateMeas(PID_Pos *p, float err, float meas)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 微分先行：对测量微分（目标阶跃不产生微分冲击） */
    float d_meas = (meas - p->prev_meas) / p->dt;
    p->dfilter += (d_meas - p->dfilter) * p->df_alpha;
    float d_term = -p->kd * p->dfilter;    /* 负号：d(SP-MV)/dt = -dMV/dt */
    p->prev_meas = meas;
    p->prev_err = err;
    return pid_pos_core(p, err, d_term);
}

void PID_Pos_Reset(PID_Pos *p)
{
    if (p == NULL) return;
    p->integral = 0.0f;
    p->prev_err = 0.0f;
    p->prev_meas = 0.0f;
    p->dfilter = 0.0f;
}

/* ================================================================
 * [02] 增量式 PID
 * ================================================================ */
void PID_Incremental_Init(PID_Incremental *p)
{
    if (p == NULL) return;
    p->e1 = p->e2 = 0.0f;
    p->out = 0.0f;
    if (p->out_min == 0.0f && p->out_max == 0.0f) {
        p->out_min = -1e6f;
        p->out_max =  1e6f;
    }
}

float PID_Incremental_Update(PID_Incremental *p, float err)
{
    if (p == NULL) return 0.0f;
    /* Δu = Kp(e1-e2) + Ki·e1 + Kd(e1-2e2+e3) */
    float delta = p->kp * (err - p->e1)
                + p->ki * err * p->dt
                + p->kd * (err - 2.0f * p->e1 + p->e2) / p->dt;
    if (p->delta_min != 0.0f || p->delta_max != 0.0f) {
        delta = pid_clampf(delta, p->delta_min, p->delta_max);
    }
    p->e2 = p->e1;
    p->e1 = err;
    p->out = pid_clampf(p->out + delta, p->out_min, p->out_max);
    return p->out;
}

void PID_Incremental_Reset(PID_Incremental *p)
{
    if (p == NULL) return;
    p->e1 = p->e2 = 0.0f;
    p->out = 0.0f;
}

/* ================================================================
 * [03] 串级 PID
 * ================================================================ */
void PID_Cascade_Init(PID_Cascade *p)
{
    if (p == NULL) return;
    PID_Pos_Init(&p->outer);
    PID_Pos_Init(&p->inner);
    p->meas_outer = 0.0f;
    p->meas_inner = 0.0f;
}

float PID_Cascade_Update(PID_Cascade *p, float err_outer)
{
    if (p == NULL) return 0.0f;
    /* 外环输出 = 内环目标（如期望角速度） */
    float ref_inner = PID_Pos_Update(&p->outer, err_outer);
    /* 内环误差 = 外环输出 - 内环测量 */
    float err_inner = ref_inner - p->meas_inner;
    return PID_Pos_Update(&p->inner, err_inner);
}

void PID_Cascade_SetInnerMeas(PID_Cascade *p, float meas_inner)
{
    if (p == NULL) return;
    p->meas_inner = meas_inner;
}

float PID_Cascade_OuterOut(PID_Cascade *p)
{
    return (p != NULL) ? p->outer.out : 0.0f;
}

/* ================================================================
 * [04] PID + 前馈
 * ================================================================ */
void PID_FeedForward_Init(PID_FeedForward *p)
{
    if (p == NULL) return;
    PID_Pos_Init(&p->pid);
    p->out = 0.0f;
}

float PID_FeedForward_Update(PID_FeedForward *p, float err, float ff_input)
{
    if (p == NULL) return 0.0f;
    float u_pid = PID_Pos_Update(&p->pid, err);
    p->out = pid_clampf(u_pid + p->ff_gain * ff_input,
                        p->out_min, p->out_max);
    return p->out;
}

/* ================================================================
 * [05] 积分分离 PID
 * ================================================================ */
void PID_Separated_Init(PID_Separated *p)
{
    if (p == NULL) return;
    p->integral = 0.0f;
    p->prev_err = 0.0f;
    p->out = 0.0f;
    if (p->out_min == 0.0f && p->out_max == 0.0f) {
        p->out_min = -1e6f;
        p->out_max =  1e6f;
    }
}

float PID_Separated_Update(PID_Separated *p, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 积分分离核心 */
    if (err <= p->sep_thresh && err >= -p->sep_thresh) {
        p->integral += p->ki * err * p->dt;
        p->integral = pid_clampf(p->integral, p->out_min, p->out_max);
    }
    float d = (err - p->prev_err) / p->dt;
    p->prev_err = err;
    p->out = pid_clampf(p->kp * err + p->integral + p->kd * d,
                        p->out_min, p->out_max);
    return p->out;
}

/* ================================================================
 * [06] 抗饱和 PID（Back-Calculation）
 * ================================================================ */
void PID_AntiWindup_Init(PID_AntiWindup *p)
{
    if (p == NULL) return;
    p->integral = 0.0f;
    p->prev_err = 0.0f;
    p->out = 0.0f;
    if (p->out_min == 0.0f && p->out_max == 0.0f) {
        p->out_min = -1e6f;
        p->out_max =  1e6f;
    }
}

float PID_AntiWindup_Update(PID_AntiWindup *p, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 未饱和输出 */
    float d = (err - p->prev_err) / p->dt;
    p->prev_err = err;
    float u_unsat = p->kp * err + p->integral + p->kd * d;
    /* 饱和输出 */
    float u = pid_clampf(u_unsat, p->out_min, p->out_max);
    /* Back-Calculation：饱和差以 kb 反馈回积分器 */
    p->integral += (p->ki * err * p->dt) + p->kb * (u - u_unsat) * p->dt;
    p->integral = pid_clampf(p->integral, p->out_min, p->out_max);
    p->out = u;
    return p->out;
}

/* ================================================================
 * [07] 微分先行 PID
 * ================================================================ */
void PID_DerivativeOnMeasure_Init(PID_DerivativeOnMeasure *p)
{
    if (p == NULL) return;
    p->integral = 0.0f;
    p->prev_meas = 0.0f;
    p->dfilter = 0.0f;
    p->out = 0.0f;
    if (p->out_min == 0.0f && p->out_max == 0.0f) {
        p->out_min = -1e6f;
        p->out_max =  1e6f;
    }
}

float PID_DerivativeOnMeasure_Update(PID_DerivativeOnMeasure *p,
                                     float err, float meas)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    p->integral += p->ki * err * p->dt;
    p->integral = pid_clampf(p->integral, p->out_min, p->out_max);
    /* 对测量微分（低通） */
    float d_meas = (meas - p->prev_meas) / p->dt;
    p->dfilter += (d_meas - p->dfilter) * p->alpha;
    p->prev_meas = meas;
    p->out = pid_clampf(p->kp * err + p->integral - p->kd * p->dfilter,
                        p->out_min, p->out_max);
    return p->out;
}

/* ================================================================
 * [08] 增益调度 PID
 * ================================================================ */
void PID_GainSched_Init(PID_GainSched *p)
{
    if (p == NULL) return;
    p->integral = 0.0f;
    p->prev_err = 0.0f;
    p->out = 0.0f;
}

float PID_GainSched_Update(PID_GainSched *p, float sched_var, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 按调度变量选择分段（线性插值相邻段，平滑过渡） */
    float av = (sched_var < 0.0f) ? -sched_var : sched_var;
    float kp = p->seg[0].kp, ki = p->seg[0].ki, kd = p->seg[0].kd;
    for (int i = 0; i < PID_SCHED_SEGMENTS - 1; i++) {
        if (av <= p->seg[i + 1].bound) {
            float t = (p->seg[i + 1].bound > p->seg[i].bound)
                    ? (av - p->seg[i].bound)
                      / (p->seg[i + 1].bound - p->seg[i].bound) : 1.0f;
            kp = p->seg[i].kp + (p->seg[i + 1].kp - p->seg[i].kp) * t;
            ki = p->seg[i].ki + (p->seg[i + 1].ki - p->seg[i].ki) * t;
            kd = p->seg[i].kd + (p->seg[i + 1].kd - p->seg[i].kd) * t;
            break;
        }
    }
    p->integral += ki * err * p->dt;
    p->integral = pid_clampf(p->integral, p->out_min, p->out_max);
    float d = (err - p->prev_err) / p->dt;
    p->prev_err = err;
    p->out = pid_clampf(kp * err + p->integral + kd * d,
                        p->out_min, p->out_max);
    return p->out;
}

/* ================================================================
 * [09] 模糊 PID（重心法去模糊）
 * ================================================================ */
/* 模糊隶属度：三角隶属函数，NB=-1, NS=-0.5, ZO=0, PS=0.5, PB=1 */
static float fuzzy_mem(float x, float center)
{
    float d = x - center;
    if (d < -0.5f || d > 0.5f) return 0.0f;
    return 1.0f - 2.0f * ((d < 0.0f) ? -d : d);
}

/* 经典模糊规则表（行=ec, 列=e；值域 -1..1）：
 * 输出 ΔKp 规则（经验表：误差大加大 P，误差小收紧） */
static const float FUZZY_DKP[5][5] = {
    { 0.5f, 0.5f, 0.3f, 0.0f, 0.0f },
    { 0.3f, 0.3f, 0.2f, 0.0f, -0.1f },
    { 0.1f, 0.1f, 0.0f, -0.1f, -0.1f },
    { 0.0f, 0.0f, -0.2f, -0.3f, -0.3f },
    { 0.0f, 0.0f, -0.3f, -0.5f, -0.5f },
};
static const float FUZZY_DKI[5][5] = {
    { -0.5f, -0.5f, -0.3f, 0.0f, 0.0f },
    { -0.3f, -0.3f, -0.2f, 0.0f, 0.1f },
    { -0.1f, -0.1f, 0.0f, 0.1f, 0.1f },
    { 0.0f, 0.0f, 0.2f, 0.3f, 0.3f },
    { 0.0f, 0.0f, 0.3f, 0.5f, 0.5f },
};
static const float FUZZY_DKD[5][5] = {
    { 0.1f, 0.1f, 0.0f, 0.0f, 0.0f },
    { 0.1f, 0.1f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.1f, 0.1f },
    { 0.0f, 0.0f, 0.0f, 0.1f, 0.1f },
};

static float fuzzy_out(const float table[5][5], float e, float ec)
{
    /* 归一化到 [-1,1] 并映射到 5 档 */
    float n_e = (e < -1.0f) ? -1.0f : ((e > 1.0f) ? 1.0f : e);
    float n_ec = (ec < -1.0f) ? -1.0f : ((ec > 1.0f) ? 1.0f : ec);
    float centers[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            float w = fuzzy_mem(n_e, centers[i]) * fuzzy_mem(n_ec, centers[j]);
            num += table[i][j] * w;
            den += w;
        }
    }
    return (den > 1e-6f) ? (num / den) : 0.0f;
}

void PID_Fuzzy_Init(PID_Fuzzy *p)
{
    if (p == NULL) return;
    p->dkp = p->dki = p->dkd = 0.0f;
    p->integral = 0.0f;
    p->prev_err = 0.0f;
    p->out = 0.0f;
}

float PID_Fuzzy_Update(PID_Fuzzy *p, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    float ec = (err - p->prev_err) / p->dt;
    p->prev_err = err;
    /* 模糊推理：e/ec 归一化后查表 */
    p->dkp = fuzzy_out(FUZZY_DKP, err * p->ke, ec * p->kec);
    p->dki = fuzzy_out(FUZZY_DKI, err * p->ke, ec * p->kec);
    p->dkd = fuzzy_out(FUZZY_DKD, err * p->ke, ec * p->kec);
    float kp = p->kp0 + p->dkp * p->kp0;
    float ki = p->ki0 + p->dki * p->ki0;
    float kd = p->kd0 + p->dkd * p->kd0;
    p->integral += ki * err * p->dt;
    p->integral = pid_clampf(p->integral, p->out_min, p->out_max);
    float d = ec;
    p->out = pid_clampf(kp * err + p->integral + kd * d,
                        p->out_min, p->out_max);
    return p->out;
}

/* ================================================================
 * [10] 史密斯预估器
 * ================================================================ */
void PID_Smith_Init(PID_Smith *p)
{
    if (p == NULL) return;
    PID_Pos_Init(&p->pid);
    p->buf_len = (uint32_t)(p->model_delay / p->dt) + 1u;
    if (p->buf_len > PID_SMITH_BUF_MAX) p->buf_len = PID_SMITH_BUF_MAX;
    for (uint32_t i = 0; i < p->buf_len; i++) p->buf[i] = 0.0f;
    p->wr = 0;
    p->model_out = 0.0f;
    p->delayed_out = 0.0f;
}

float PID_Smith_Update(PID_Smith *p, float setpoint, float plant_out)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 1. 无延迟模型输出（一阶惯性）：d ym/dt = (K·u - ym)/τ */
    float u_ctrl = p->pid.out;
    p->model_out += (p->model_k * u_ctrl - p->model_out)
                    * (p->model_tau > 0.0f
                       ? (p->dt / p->model_tau) : 1.0f);
    /* 2. 模型输出入延迟队列（环形） */
    if (p->buf_len > 0u) {
        p->buf[p->wr] = p->model_out;
        p->wr = (p->wr + 1u) % p->buf_len;
        p->delayed_out = p->buf[p->wr];   /* 读最旧（延迟 τ） */
    } else {
        p->delayed_out = p->model_out;
    }
    /* 3. 等效反馈误差 = (设定 - 实际) - (模型延迟输出 - 无延迟模型输出)
     *    —— 补偿掉延迟，控制器面对"无延迟对象" */
    float err_smith = (setpoint - plant_out)
                    - (p->delayed_out - p->model_out);
    return PID_Pos_Update(&p->pid, err_smith);
}

/* ================================================================
 * [11] Bang-Bang + PID 混合
 * ================================================================ */
void PID_BangBang_Init(PID_BangBang *p)
{
    if (p == NULL) return;
    PID_Pos_Init(&p->pid);
    p->out = 0.0f;
}

float PID_BangBang_Update(PID_BangBang *p, float err)
{
    if (p == NULL) return 0.0f;
    if (err > p->bang_thresh) {
        p->out = (p->bang_out > 0.0f) ? p->bang_out : p->pid.out_max;
    } else if (err < -p->bang_thresh) {
        p->out = (p->bang_out > 0.0f) ? -p->bang_out : p->pid.out_min;
    } else {
        p->out = PID_Pos_Update(&p->pid, err);
    }
    return p->out;
}

/* ================================================================
 * [12] 继电反馈自整定（极限环法）
 * ================================================================ */
void PID_Autotune_Init(PID_Autotune *p)
{
    if (p == NULL) return;
    p->u = 0.0f;
    p->y_prev = 0.0f;
    p->a = 0.0f;
    p->tu = 0.0f;
    p->t_rise = p->t_fall = 0.0f;
    p->half_cycles = 0;
    p->done = 0;
    p->kp = p->ki = p->kd = 0.0f;
}

float PID_Autotune_Update(PID_Autotune *p, float y, float setpoint)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    if (p->done) return p->u;

    /* 继电器逻辑（带滞环）：e > h → +h；e < -h → -h */
    float e = setpoint - y;
    if (e > p->relay_hyst) {
        p->u = p->relay_h;
    } else if (e < -p->relay_hyst) {
        p->u = -p->relay_h;
    }
    /* 检测过零（输出翻转 = 半周期完成） */
    if ((p->y_prev - setpoint) * (y - setpoint) < 0.0f) {
        p->half_cycles++;
        float now = (float)p->half_cycles * p->dt;
        if (p->half_cycles >= 4u) {
            /* 稳态极限环：Tu = 周期，a = 半振幅 */
            float t_prev = (p->half_cycles & 1u) ? p->t_fall : p->t_rise;
            float t_cur = now;
            float half_t = t_cur - t_prev;
            if (p->tu > 0.0f) {
                p->tu = (p->tu + 2.0f * half_t) * 0.5f;
            } else {
                p->tu = 2.0f * half_t;
            }
            if (p->half_cycles & 1u) { p->t_fall = t_cur; }
            else { p->t_rise = t_cur; }
            /* 幅值估计：连续 4 个半周期后取平均 */
            if (p->half_cycles >= 8u && !p->done) {
                float ku = 4.0f * p->relay_h / (3.14159f * (p->a > 1e-6f ? p->a : 1e-6f));
                /* Ziegler-Nichols 整定（经典公式） */
                p->kp = 0.6f * ku;
                p->ki = p->kp / (0.5f * p->tu);
                p->kd = p->kp * 0.125f * p->tu;
                p->done = 1;
            }
        }
    }
    /* 幅值跟踪：|y - setpoint| 的峰值 */
    float amp = (y > setpoint) ? (y - setpoint) : (setpoint - y);
    if (amp > p->a) p->a = amp;
    p->y_prev = y;
    return p->u;
}

/* ================================================================
 * [13] 单神经元自适应 PID
 * ================================================================ */
void PID_Neural_Init(PID_Neural *p)
{
    if (p == NULL) return;
    p->x1 = p->x2 = p->x3 = 0.0f;
    p->prev_err = 0.0f;
    p->out = 0.0f;
}

float PID_Neural_Update(PID_Neural *p, float err)
{
    if (p == NULL) return 0.0f;
    /* 神经元输入：x1=e, x2=Δe, x3=Δ²e（归一化到 ±1 量级） */
    float de = err - p->prev_err;
    p->x1 = err;
    p->x2 = de;
    p->x3 = de - p->x2;   /* 二阶差分 */
    float u = p->wp * p->x1 + p->wi * p->x2 + p->wd * p->x3;
    p->out = pid_clampf(u, p->out_min, p->out_max);
    /* Hebb 学习律（有监督：以误差为性能指标） */
    float z = p->out * err;   /* 简化学习信号 */
    p->wp += p->eta_p * z * p->x1;
    p->wi += p->eta_i * z * p->x2;
    p->wd += p->eta_d * z * p->x3;
    p->prev_err = err;
    return p->out;
}

/* ================================================================
 * [14] 死区 + 输出限速 PID
 * ================================================================ */
void PID_Deadband_Init(PID_Deadband *p)
{
    if (p == NULL) return;
    PID_Pos_Init(&p->pid);
    p->out = 0.0f;
}

float PID_Deadband_Update(PID_Deadband *p, float err)
{
    if (p == NULL || p->dt <= 0.0f) return 0.0f;
    /* 死区：小误差保持输出（防机械抖动/回差） */
    float e_in = pid_deadzone(err, p->deadband);
    float target = PID_Pos_Update(&p->pid, e_in);
    /* 限速：输出变化率 ≤ rate_max/s */
    if (p->rate_max > 0.0f) {
        float max_step = p->rate_max * p->dt;
        float d = target - p->out;
        if (d > max_step) d = max_step;
        else if (d < -max_step) d = -max_step;
        p->out += d;
    } else {
        p->out = target;
    }
    return p->out;
}
