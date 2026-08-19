/* ================================================================
 * pid.h —— PID 控制算法全家族（14 变式）
 *
 * 架构位置：APP 应用层 ctrl/ 子模块；纯 C、float 运算（FPU 加速）、
 *           结构体实例化（可重入、多实例、无动态内存）。
 *
 * 通用约定：
 *   - 全部函数返回 void，状态全部在实例结构体内（线程安全由调用方保证）；
 *   - dt 为采样周期（秒），必须在 update 前初始化；
 *   - 输出/积分限幅按控制对象量纲填写；
 *   - 未用的增益/参数置 0 即退化为更简形式（如 kd=0 → PI）。
 *
 * 变式索引（应用选型速查）：
 *   [01] PID_Pos          位置式（最基础，一切之母）
 *   [02] PID_Incremental  增量式（输出=Δu，舵机/阀门防冲击）
 *   [03] PID_Cascade      串级（外环位置 → 内环速度，云台/平衡车标准架构）
 *   [04] PID_FeedForward  前馈（扰动已知时提前补偿，跟踪性能核心）
 *   [05] PID_Separated    积分分离（大偏差禁积分，防超调）
 *   [06] PID_AntiWindup   抗饱和（back-calculation，防积分饱和）
 *   [07] PID_DerivativeOnMeasure 微分先行（对测量微分，抑制噪声放大）
 *   [08] PID_GainSched    增益调度（分段 Kp/Ki/Kd，非线性对象分段线性化）
 *   [09] PID_Fuzzy        模糊 PID（模糊规则在线修正增益，免精确建模）
 *   [10] PID_Smith        史密斯预估（大纯滞后对象，如视觉 30-100ms 延迟）
 *   [11] PID_BangBang     双位式+PID 混合（大误差粗调、小误差精调）
 *   [12] PID_Autotune     继电反馈自整定（极限环法自动整定 PID）
 *   [13] PID_Neural       单神经元自适应 PID（在线学习增益）
 *   [14] PID_Deadband     死区 + 限速输出（机械回差/执行器保护）
 *
 * 使用场景索引：
 *   云台/电机速度环 → 03/02；视觉伺服（带延迟）→ 10+04；
 *   温控 → 05+06；四旋翼角速度环 → 03；非线性对象 → 08/09/13。
 * ================================================================ */
#ifndef PID_H
#define PID_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * [01] 位置式 PID（全特性版）
 *
 *  u(t) = Kp·e(t) + Ki·∫e(τ)dτ + Kd·de(t)/dt
 *
 * 本结构体集积分分离、微分低通、输出/积分限幅于一体；
 * 关闭对应特性即退化为经典位置式。
 * 场景：温度/压力/位置等慢回路；教学基准。
 * ---------------------------------------------------------------- */
typedef struct {
    /* ---- 增益（用户配置） ---- */
    float kp;               /* 比例增益：当前误差响应强度 */
    float ki;               /* 积分增益：消除稳态误差 */
    float kd;               /* 微分增益：抑制超调/预测趋势 */
    float dt;               /* 采样周期（秒） */
    float out_min, out_max; /* 输出限幅（执行器量纲） */
    float int_min, int_max; /* 积分限幅（抗积分饱和） */
    float sep_thresh;       /* 积分分离阈值：|e|>thresh 时禁积分（0=禁用） */
    float df_alpha;         /* 微分低通系数 0~1（0=纯微分，1=完全平滑） */

    /* ---- 内部状态 ---- */
    float integral;         /* 积分累加值 */
    float prev_err;         /* 上周期误差（微分项） */
    float prev_meas;        /* 上周期测量值（微分先行用） */
    float dfilter;          /* 微分低通滤波状态 */
    float out;              /* 最近输出 */
} PID_Pos;

/* 初始化：清零状态，设置默认限幅（±1e6） */
void PID_Pos_Init(PID_Pos *p);

/* 一步更新：err = 目标 - 测量；返回本次输出 */
float PID_Pos_Update(PID_Pos *p, float err);

/* 带测量输入的更新（启用微分先行时用真实测量而非误差微分） */
float PID_Pos_UpdateMeas(PID_Pos *p, float err, float meas);

/* 复位内部状态（不清增益/限幅） */
void PID_Pos_Reset(PID_Pos *p);

/* ----------------------------------------------------------------
 * [02] 增量式 PID
 *
 *  Δu = Kp·(e_k - e_{k-1}) + Ki·e_k + Kd·(e_k - 2e_{k-1} + e_{k-2})
 *  u_k = u_{k-1} + Δu
 *
 * 输出为增量而非绝对值 → 对执行器无冲击、天然抗积分饱和、
 * 手/自动无扰切换。场景：舵机、阀门、步进电机速度。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp, ki, kd;       /* 增益 */
    float dt;
    float out_min, out_max; /* 绝对输出限幅（最终输出） */
    float delta_min, delta_max; /* 单步增量限幅（执行器速率保护） */
    float e1, e2;           /* 历史误差 e_{k-1}, e_{k-2} */
    float out;              /* 最近绝对输出 */
} PID_Incremental;

void PID_Incremental_Init(PID_Incremental *p);
float PID_Incremental_Update(PID_Incremental *p, float err);
void PID_Incremental_Reset(PID_Incremental *p);

/* ----------------------------------------------------------------
 * [03] 串级 PID（位置环 × 速度环）
 *
 *  外环误差 → 外环PID → 内环目标值 → 内环PID → 执行器
 *  内环（快）先闭合，外环（慢）包络——带宽分离、抗扰能力倍增。
 * 场景：云台（角度环+角速度环）、平衡车（角度环+角速度环）、
 *       四旋翼（角速度环+姿态环）——工业标准架构。
 * ---------------------------------------------------------------- */
typedef struct {
    PID_Pos outer;          /* 外环（位置/角度）——输出作为内环目标 */
    PID_Pos inner;          /* 内环（速度/角速度）——直接驱动执行器 */
    float meas_outer;       /* 外环测量（如角度） */
    float meas_inner;       /* 内环测量（如角速度） */
} PID_Cascade;

void PID_Cascade_Init(PID_Cascade *p);
/* 一步串级更新：err_outer = 外环目标 - 外环测量；
 * 内环测量由调用方通过 PID_Cascade_SetInnerMeas 注入。 */
float PID_Cascade_Update(PID_Cascade *p, float err_outer);
void PID_Cascade_SetInnerMeas(PID_Cascade *p, float meas_inner);
float PID_Cascade_OuterOut(PID_Cascade *p);   /* 查看外环输出（内环目标） */

/* ----------------------------------------------------------------
 * [04] PID + 前馈（FeedForward）
 *
 *  u = u_pid + u_ff，u_ff = ff_gain × 目标速度（或已知扰动模型）
 *
 * 前馈不依赖反馈，提前动作——对已知/可测扰动是"降维打击"。
 * 场景：视觉目标速度前馈（目标移动时云台提前转向）、
 *       重力补偿前馈（TILT 轴）、电源前馈。
 * ---------------------------------------------------------------- */
typedef struct {
    PID_Pos pid;            /* 反馈部分（全特性位置式） */
    float ff_gain;          /* 前馈增益 */
    float out;              /* 总输出 = 反馈 + 前馈（限幅外） */
    float out_min, out_max; /* 总输出限幅 */
} PID_FeedForward;

void PID_FeedForward_Init(PID_FeedForward *p);
/* err：反馈误差；ff_input：前馈输入（如目标速度） */
float PID_FeedForward_Update(PID_FeedForward *p, float err, float ff_input);

/* ----------------------------------------------------------------
 * [05] 积分分离 PID
 *
 *  |e| ≤ sep_thresh → 正常积分；|e| > sep_thresh → 积分冻结。
 *  大偏差阶段（阶跃启动/目标突变）防止积分累积导致超调。
 *  场景：温控大阶跃、云台快速捕获目标、任何大幅阶跃回路。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp, ki, kd;
    float dt;
    float sep_thresh;       /* 分离阈值（>0 启用） */
    float out_min, out_max;
    float integral;
    float prev_err;
    float out;
} PID_Separated;

void PID_Separated_Init(PID_Separated *p);
float PID_Separated_Update(PID_Separated *p, float err);

/* ----------------------------------------------------------------
 * [06] 抗饱和 PID（Back-Calculation）
 *
 *  当输出进入饱和，把"实际输出与未饱和输出的差"反馈回积分器
 *  （u_out - u_unsat）× kb 项，积分器被"拉回"——比单纯积分限幅
 *  更平滑、恢复更快。场景：执行器饱和频繁的回路（大负载）。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp, ki, kd;
    float dt;
    float kb;               /* 反算增益（通常 = ki 的 1~5 倍） */
    float out_min, out_max;
    float integral;
    float prev_err;
    float out;              /* 饱和后实际输出 */
} PID_AntiWindup;

void PID_AntiWindup_Init(PID_AntiWindup *p);
float PID_AntiWindup_Update(PID_AntiWindup *p, float err);

/* ----------------------------------------------------------------
 * [07] 微分先行 PID（Derivative-on-Measure）
 *
 *  微分作用于测量值而非误差：目标阶跃时微分项不跳变（无微分冲击），
 *  测量噪声被一阶低通抑制。场景：目标频繁阶跃 + 噪声传感器
 *  （视觉测量、电位器）。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp, ki, kd;
    float dt;
    float alpha;            /* 测量微分低通 0~1 */
    float out_min, out_max;
    float integral;
    float prev_meas;        /* 上次测量 */
    float dfilter;          /* 微分滤波状态 */
    float out;
} PID_DerivativeOnMeasure;

void PID_DerivativeOnMeasure_Init(PID_DerivativeOnMeasure *p);
float PID_DerivativeOnMeasure_Update(PID_DerivativeOnMeasure *p, float err, float meas);

/* ----------------------------------------------------------------
 * [08] 增益调度 PID（Gain Scheduling）
 *
 *  按调度变量（误差大小/目标距离/速度档位）分段查表增益——
 *  非线性对象的分段线性化控制。场景：云台远近目标变增益、
 *  飞行器动压调度、机械臂变负载。
 * ---------------------------------------------------------------- */
#define PID_SCHED_SEGMENTS   4

typedef struct {
    struct {
        float bound;        /* 调度变量上界（升序） */
        float kp, ki, kd;
    } seg[PID_SCHED_SEGMENTS];
    float dt;
    float out_min, out_max;
    float integral;
    float prev_err;
    float out;
} PID_GainSched;

void PID_GainSched_Init(PID_GainSched *p);
/* sched_var：调度变量（如 |目标距离|）；err：控制误差 */
float PID_GainSched_Update(PID_GainSched *p, float sched_var, float err);

/* ----------------------------------------------------------------
 * [09] 模糊 PID
 *
 *  以误差 e 与误差变化率 ec 的模糊化（NB/NS/ZO/PS/PB）查规则表，
 *  输出 ΔKp/ΔKi/ΔKd 修正基础增益——免精确建模的智能整定。
 *  场景：强非线性/难建模对象（欠阻尼负载、变惯量）。
 *  注：本实现采用重心法去模糊，规则表为经典 5×5 经验表。
 * ---------------------------------------------------------------- */
typedef struct {
    float kp0, ki0, kd0;    /* 基础增益 */
    float ke, kec;          /* 归一化系数（e/ec 映射到 [-1,1]） */
    float dkp, dki, dkd;    /* 模糊输出修正量 */
    float dt;
    float out_min, out_max;
    float integral;
    float prev_err;
    float out;
} PID_Fuzzy;

void PID_Fuzzy_Init(PID_Fuzzy *p);
float PID_Fuzzy_Update(PID_Fuzzy *p, float err);

/* ----------------------------------------------------------------
 * [10] 史密斯预估器（Smith Predictor）
 *
 *  对象 = 无延迟模型 G(s) + 纯滞后 e^(-τs)：
 *  用模型输出补偿反馈，使控制器面对"无延迟等效对象"——
 *  视觉链路 30-100ms 延迟、传输管道滞后等大纯滞后场景的经典解法。
 *  模型：一阶惯性 + 延迟缓冲队列。
 * ---------------------------------------------------------------- */
typedef struct {
    PID_Pos pid;            /* 主控制器（针对无延迟模型整定） */
    float model_k;          /* 模型增益 */
    float model_tau;        /* 模型时间常数（秒） */
    float model_delay;      /* 模型纯滞后（秒） */
    float dt;
    /* 延迟缓冲（环形）：容量 = ceil(model_delay/dt) + 1 */
#define PID_SMITH_BUF_MAX   64
    float buf[PID_SMITH_BUF_MAX];
    uint32_t buf_len;       /* 实际缓冲长度（由 delay/dt 决定） */
    uint32_t wr;            /* 写指针 */
    float model_out;        /* 模型输出 */
    float delayed_out;      /* 延迟后的模型输出 */
} PID_Smith;

void PID_Smith_Init(PID_Smith *p);
/* plant_out：实际被控量测量（用于模型校正）；setpoint：设定值 */
float PID_Smith_Update(PID_Smith *p, float setpoint, float plant_out);

/* ----------------------------------------------------------------
 * [11] Bang-Bang + PID 混合
 *
 *  |e| > bang_thresh → 满量程输出（快速趋近）；
 *  |e| ≤ bang_thresh → PID 精调（防振荡）。
 *  场景：快速捕获目标（云台大角度转向）、双模温控。
 * ---------------------------------------------------------------- */
typedef struct {
    PID_Pos pid;            /* 精调段 PID */
    float bang_thresh;      /* 切换阈值 */
    float bang_out;         /* 粗调段满量程输出（带符号取） */
    float out;
} PID_BangBang;

void PID_BangBang_Init(PID_BangBang *p);
float PID_BangBang_Update(PID_BangBang *p, float err);

/* ----------------------------------------------------------------
 * [12] 继电反馈自整定（Åström–Hägglund 极限环法）
 *
 *  继电器输出迫使系统进入极限环振荡，测得极限增益 Ku 与
 *  极限周期 Tu → Ziegler-Nichols 公式自动整定 Kp/Ki/Kd。
 *  场景：上电一键自整定（云台/温控参数自动获取）。
 * ---------------------------------------------------------------- */
typedef struct {
    float relay_h;          /* 继电器幅值 */
    float relay_hyst;       /* 继电器滞环（防噪声误翻转） */
    float dt;
    /* 整定过程状态 */
    float u;                /* 当前继电器输出 */
    float y_prev;           /* 上次测量 */
    float a;                /* 振荡幅值估计 */
    float tu;               /* 极限周期（秒） */
    float t_rise, t_fall;   /* 上升/下降沿时刻 */
    uint32_t half_cycles;   /* 完成的半周期数 */
    uint8_t  done;          /* 整定完成标志 */
    /* 整定结果 */
    float kp, ki, kd;
} PID_Autotune;

void PID_Autotune_Init(PID_Autotune *p);
/* 每次采样调用；done=1 后读取 kp/ki/kd */
float PID_Autotune_Update(PID_Autotune *p, float y, float setpoint);

/* ----------------------------------------------------------------
 * [13] 单神经元自适应 PID
 *
 *  把 PID 三项看作神经元权重 w=[wp,wi,wd]，按 Hebb 学习律在线
 *  修正（学习率 ηp/ηi/ηd）——免人工整定的在线自适应。
 *  场景：参数慢变对象、自学习演示。
 * ---------------------------------------------------------------- */
typedef struct {
    float wp, wi, wd;       /* 神经元权重（初始=经典增益） */
    float eta_p, eta_i, eta_d; /* 学习率（0.01~0.1 量级） */
    float dt;
    float out_min, out_max;
    float x1, x2, x3;       /* 神经元输入：e, Δe, Δ²e */
    float prev_err;
    float out;
} PID_Neural;

void PID_Neural_Init(PID_Neural *p);
float PID_Neural_Update(PID_Neural *p, float err);

/* ----------------------------------------------------------------
 * [14] 死区 + 输出限速 PID
 *
 *  |e| < deadband → 输出保持（机械回差/静区消除）；
 *  输出变化率 ≤ rate_max/s（执行器速率保护）。
 *  场景：舵机回差补偿、液压阀、任何机械执行器。
 * ---------------------------------------------------------------- */
typedef struct {
    PID_Pos pid;
    float deadband;         /* 误差死区 */
    float rate_max;         /* 输出变化率上限（单位/s；0=禁用） */
    float dt;
    float out;
} PID_Deadband;

void PID_Deadband_Init(PID_Deadband *p);
float PID_Deadband_Update(PID_Deadband *p, float err);

/* ================================================================
 * 通用辅助（全部变式共用）
 * ================================================================ */
/* 限幅 */
static inline float pid_clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* 死区 */
static inline float pid_deadzone(float v, float dz)
{
    return (v > dz) ? (v - dz) : ((v < -dz) ? (v + dz) : 0.0f);
}

#ifdef __cplusplus
}
#endif

#endif /* PID_H */
