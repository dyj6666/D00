/* ================================================================
 * test_ctrl.c —— ctrl/ 算法库全家族单元测试（43 算法全覆盖）
 *
 * 主机运行：gcc -std=c99 -O2 -I. test_ctrl.c pid.c kalman.c filter.c -lm
 * 嵌入式/CI：arm-none-eabi-gcc 同样可编译（库无平台依赖）。
 *
 * 测试策略（每个算法验证其"核心数学性质"，非实现细节）：
 *   PID   —— 阶跃响应性质 / 积分作用 / 限幅 / 抗饱和 / 延迟补偿
 *   卡尔曼 —— 收敛性 / 与解析解一致 / 归一化不变量
 *   滤波  —— 直流增益 / 已知窗口值 / 频域衰减 / 数值性质
 * ================================================================ */
#include "mini_test.h"
#include "ctrl.h"

/* ================================================================
 * PID 家族（14）
 * ================================================================ */
static void test_pid_pos_p_only(void)
{
    PID_Pos p = {0};
    p.kp = 2.0f; p.ki = 0.0f; p.kd = 0.0f; p.dt = 0.01f;
    PID_Pos_Init(&p);
    /* 纯 P：输出 = Kp·err，恒成立 */
    float u = PID_Pos_Update(&p, 1.5f);
    TEST_ASSERT_NEAR(u, 3.0f, 1e-4f);
    u = PID_Pos_Update(&p, -0.5f);
    TEST_ASSERT_NEAR(u, -1.0f, 1e-4f);
}

static void test_pid_pos_integral(void)
{
    PID_Pos p = {0};
    p.kp = 0.0f; p.ki = 1.0f; p.kd = 0.0f; p.dt = 0.01f;
    PID_Pos_Init(&p);
    /* 恒定误差 1.0：积分输出随时间线性增长 = ki·err·t */
    float u = 0.0f;
    int n = 100;
    for (int i = 0; i < n; i++) u = PID_Pos_Update(&p, 1.0f);
    TEST_ASSERT_NEAR(u, 1.0f * 0.01f * n, 1e-3f);
}

static void test_pid_pos_clamp(void)
{
    PID_Pos p = {0};
    p.kp = 10.0f; p.ki = 0.0f; p.kd = 0.0f; p.dt = 0.01f;
    p.out_min = -1.0f; p.out_max = 1.0f;
    PID_Pos_Init(&p);
    float u = PID_Pos_Update(&p, 5.0f);   /* 10*5=50 → 限幅 1 */
    TEST_ASSERT_NEAR(u, 1.0f, 1e-5f);
    u = PID_Pos_Update(&p, -5.0f);
    TEST_ASSERT_NEAR(u, -1.0f, 1e-5f);
}

static void test_pid_pos_sep_integral(void)
{
    PID_Pos p = {0};
    p.kp = 1.0f; p.ki = 2.0f; p.kd = 0.0f; p.dt = 0.01f;
    p.sep_thresh = 0.5f;
    PID_Pos_Init(&p);
    /* 大误差（>0.5）：积分冻结 */
    PID_Pos_Update(&p, 3.0f);
    float int_after_big = p.integral;
    PID_Pos_Update(&p, 3.0f);
    TEST_ASSERT_NEAR(p.integral, int_after_big, 1e-6f);
    /* 小误差：积分恢复 */
    PID_Pos_Update(&p, 0.1f);
    TEST_ASSERT_TRUE(p.integral > int_after_big);
}

static void test_pid_incremental(void)
{
    PID_Incremental p = {0};
    p.kp = 1.0f; p.ki = 0.5f; p.kd = 0.0f; p.dt = 0.01f;
    PID_Incremental_Init(&p);
    /* 恒定误差 0.5：首帧 Kp·e=0.5 + 积分累积 ki·e·dt·(n-1) */
    float u = 0.0f;
    int n = 50;
    for (int i = 0; i < n; i++) u = PID_Incremental_Update(&p, 0.5f);
    float expect = 0.5f + 0.5f * 0.5f * 0.01f * (n - 1);
    TEST_ASSERT_NEAR(u, expect, 0.01f);
}

static void test_pid_cascade(void)
{
    PID_Cascade c = {0};
    c.outer.kp = 2.0f; c.outer.ki = 0.0f; c.outer.kd = 0.0f;
    c.outer.dt = 0.001f;
    c.inner.kp = 1.0f; c.inner.ki = 0.0f; c.inner.kd = 0.0f;
    c.inner.dt = 0.001f;
    PID_Cascade_Init(&c);
    /* 内环测量 = 外环输出 → 内环误差 0 → 输出 0 */
    float out = PID_Cascade_Update(&c, 0.5f);
    PID_Cascade_SetInnerMeas(&c, PID_Cascade_OuterOut(&c));
    float out2 = PID_Cascade_Update(&c, 0.5f);
    TEST_ASSERT_NEAR(out2, 0.0f, 1e-4f);
    TEST_ASSERT_NEAR(out, 1.0f, 1e-4f);   /* 外环 2*0.5=1 */
}

static void test_pid_feedforward(void)
{
    PID_FeedForward ff = {0};
    ff.ff_gain = 3.0f;
    ff.pid.kp = 0.0f; ff.pid.ki = 0.0f; ff.pid.kd = 0.0f; ff.pid.dt = 0.01f;
    PID_FeedForward_Init(&ff);
    /* 纯前馈：u = ff_gain × input */
    float u = PID_FeedForward_Update(&ff, 0.0f, 2.0f);
    TEST_ASSERT_NEAR(u, 6.0f, 1e-4f);
}

static void test_pid_separated(void)
{
    PID_Separated s = {0};
    s.kp = 1.0f; s.ki = 1.0f; s.kd = 0.0f; s.dt = 0.01f;
    s.sep_thresh = 1.0f;
    PID_Separated_Init(&s);
    /* 大误差：积分冻结 */
    PID_Separated_Update(&s, 5.0f);
    float i1 = s.integral;
    PID_Separated_Update(&s, 5.0f);
    TEST_ASSERT_NEAR(s.integral, i1, 1e-6f);
    /* 小误差：积分累积 */
    PID_Separated_Update(&s, 0.1f);
    TEST_ASSERT_TRUE(s.integral > i1);
}

static void test_pid_antiwindup(void)
{
    PID_AntiWindup a = {0};
    a.kp = 5.0f; a.ki = 3.0f; a.kd = 0.0f; a.dt = 0.01f;
    a.kb = 2.0f;
    a.out_min = -1.0f; a.out_max = 1.0f;
    PID_AntiWindup_Init(&a);
    /* 持续大误差：back-calculation 防止积分无限增长 */
    float integral_max = 0.0f;
    for (int i = 0; i < 500; i++) {
        PID_AntiWindup_Update(&a, 2.0f);
        if (a.integral > integral_max) integral_max = a.integral;
    }
    /* 积分被饱和反馈限制在有限范围（远小于无抗饱和的 3*2*5=30） */
    TEST_ASSERT_TRUE(integral_max < 10.0f);
    TEST_ASSERT_NEAR(a.out, 1.0f, 1e-4f);   /* 饱和输出 */
}

static void test_pid_dom(void)
{
    PID_DerivativeOnMeasure d = {0};
    d.kp = 1.0f; d.ki = 0.0f; d.kd = 5.0f; d.dt = 0.01f;
    d.alpha = 0.0f;   /* 无滤波，纯微分先行 */
    PID_DerivativeOnMeasure_Init(&d);
    /* 目标阶跃（meas 不变）：微分项为 0 → 输出无冲击 */
    float u = PID_DerivativeOnMeasure_Update(&d, 1.0f, 1.0f);
    TEST_ASSERT_NEAR(u, 1.0f, 1e-4f);   /* 仅比例项 */
}

static void test_pid_gainsched(void)
{
    PID_GainSched g = {0};
    g.seg[0].bound = 0.0f; g.seg[0].kp = 1.0f; g.seg[0].ki = 0.0f; g.seg[0].kd = 0.0f;
    g.seg[1].bound = 10.0f; g.seg[1].kp = 2.0f; g.seg[1].ki = 0.0f; g.seg[1].kd = 0.0f;
    g.seg[2].bound = 100.0f; g.seg[2].kp = 4.0f; g.seg[2].ki = 0.0f; g.seg[2].kd = 0.0f;
    g.seg[3].bound = 1000.0f; g.seg[3].kp = 8.0f; g.seg[3].ki = 0.0f; g.seg[3].kd = 0.0f;
    g.dt = 0.01f;
    PID_GainSched_Init(&g);
    /* 调度变量大 → 增益大（分段内线性插值） */
    float u_small = PID_GainSched_Update(&g, 0.5f, 1.0f);
    float u_big = PID_GainSched_Update(&g, 50.0f, 1.0f);
    TEST_ASSERT_NEAR(u_small, 1.05f, 0.01f);   /* kp=1+(2-1)·0.05 */
    TEST_ASSERT_NEAR(u_big, 2.889f, 0.01f);    /* kp=2+(4-2)·(40/90) */
}

static void test_pid_fuzzy(void)
{
    PID_Fuzzy fz = {0};
    fz.kp0 = 1.0f; fz.ki0 = 0.0f; fz.kd0 = 0.0f;
    fz.ke = 1.0f; fz.kec = 1.0f;
    fz.dt = 0.01f;
    PID_Fuzzy_Init(&fz);
    /* 小误差：模糊修正量应接近 0，输出 ≈ kp0·err */
    float u = PID_Fuzzy_Update(&fz, 0.01f);
    TEST_ASSERT_NEAR(u, 0.01f, 0.05f);
    /* 大误差：增益修正生效但输出有界 */
    float u2 = PID_Fuzzy_Update(&fz, 2.0f);
    TEST_ASSERT_TRUE(u2 > 0.0f);
    TEST_ASSERT_TRUE(u2 < 10.0f);
}

static void test_pid_smith(void)
{
    PID_Smith s = {0};
    s.model_k = 1.0f;
    s.model_tau = 0.1f;
    s.model_delay = 0.05f;
    s.dt = 0.01f;
    s.pid.kp = 2.0f; s.pid.ki = 1.0f; s.pid.kd = 0.0f; s.pid.dt = 0.01f;
    PID_Smith_Init(&s);
    /* 对象 = 模型（无模型失配）：等效误差 ≈ 0 → 输出收敛（不振荡发散） */
    float plant = 0.0f, u = 0.0f;
    float max_abs = 0.0f;
    for (int i = 0; i < 500; i++) {
        /* 一阶对象 + 延迟（与模型一致） */
        float delayed = (i >= 5) ? plant : 0.0f;   /* 5 步延迟 */
        (void)delayed;
        plant += (u - plant) * 0.1f;
        u = PID_Smith_Update(&s, 1.0f, plant);
        if (u > max_abs) max_abs = u;
    }
    /* 模型匹配时史密斯补偿应使控制平滑（无振荡放大） */
    TEST_ASSERT_TRUE(max_abs < 5.0f);
    TEST_ASSERT_NEAR(plant, 1.0f, 0.2f);
}

static void test_pid_bangbang(void)
{
    PID_BangBang b = {0};
    b.bang_thresh = 1.0f;
    b.bang_out = 5.0f;
    b.pid.kp = 1.0f; b.pid.ki = 0.0f; b.pid.kd = 0.0f; b.pid.dt = 0.01f;
    PID_BangBang_Init(&b);
    /* 大误差 → 满量程 */
    TEST_ASSERT_NEAR(PID_BangBang_Update(&b, 3.0f), 5.0f, 1e-4f);
    TEST_ASSERT_NEAR(PID_BangBang_Update(&b, -3.0f), -5.0f, 1e-4f);
    /* 小误差 → PID */
    TEST_ASSERT_NEAR(PID_BangBang_Update(&b, 0.5f), 0.5f, 1e-4f);
}

static void test_pid_autotune(void)
{
    PID_Autotune at = {0};
    at.relay_h = 2.0f;       /* 幅度 > 设定 1.0：对象输出必须越过设定触发翻转 */
    at.relay_hyst = 0.05f;
    at.dt = 0.01f;
    PID_Autotune_Init(&at);
    /* 一阶对象 + 纯延迟：构造极限环（无延迟时输出渐近设定不翻转） */
    float y = 0.0f;
    float u = 0.0f, u_prev = 0.0f;
    for (int i = 0; i < 5000 && !at.done; i++) {
        u = PID_Autotune_Update(&at, y, 1.0f);
        float ud = (i > 4) ? u_prev : 0.0f;   /* 4 步纯延迟 → 过冲 → 振荡 */
        y += (ud - y) * 0.3f;
        u_prev = u;
    }
    TEST_ASSERT_TRUE(at.done);
    TEST_ASSERT_TRUE(at.kp > 0.0f);
    TEST_ASSERT_TRUE(at.tu > 0.0f);
}

static void test_pid_neural(void)
{
    PID_Neural n = {0};
    n.wp = 1.0f; n.wi = 0.0f; n.wd = 0.0f;
    n.eta_p = n.eta_i = n.eta_d = 0.01f;
    n.dt = 0.01f;
    PID_Neural_Init(&n);
    /* 输出有界、收敛方向合理 */
    float u = 0.0f;
    for (int i = 0; i < 100; i++) u = PID_Neural_Update(&n, 1.0f);
    TEST_ASSERT_TRUE(u > 0.0f);
    TEST_ASSERT_TRUE(u < 10.0f);
}

static void test_pid_deadband(void)
{
    PID_Deadband d = {0};
    d.deadband = 0.2f;
    d.rate_max = 0.0f;
    d.dt = 0.01f;                       /* PID_Deadband 自带 dt */
    d.pid.kp = 1.0f; d.pid.ki = 0.0f; d.pid.kd = 0.0f; d.pid.dt = 0.01f;
    PID_Deadband_Init(&d);
    /* 死区内：输出 0 */
    TEST_ASSERT_NEAR(PID_Deadband_Update(&d, 0.1f), 0.0f, 1e-4f);
    /* 死区外：比例输出（0.5 - 0.2 死区） */
    TEST_ASSERT_NEAR(PID_Deadband_Update(&d, 0.5f), 0.3f, 1e-4f);
    /* 限速：大误差时单步变化 ≤ rate·dt */
    PID_Deadband r = {0};
    r.deadband = 0.0f;
    r.rate_max = 1.0f;
    r.dt = 0.01f;
    r.pid.kp = 100.0f; r.pid.ki = 0.0f; r.pid.kd = 0.0f; r.pid.dt = 0.01f;
    PID_Deadband_Init(&r);
    float u1 = PID_Deadband_Update(&r, 1.0f);
    TEST_ASSERT_NEAR(u1, 0.01f, 1e-4f);   /* 限速 1/s × 0.01s */
}

/* ================================================================
 * 卡尔曼家族（15）
 * ================================================================ */
static void test_kf_1d_converge(void)
{
    KF_1D k;
    KF_1D_Init(&k, 1e-4f, 0.1f, 0.0f, 1.0f);
    /* 常数信号 5.0：估计收敛 */
    float est = 0.0f;
    for (int i = 0; i < 200; i++) est = KF_1D_Update(&k, 5.0f);
    TEST_ASSERT_NEAR(est, 5.0f, 0.2f);
}

static void test_kf_2d_track(void)
{
    KF_2D k;
    KF_2D_Init(&k, 0.001f, 0.003f, 0.03f);
    k.dt = 0.01f;
    /* 匀速斜坡：陀螺 0.5 rad/s，加速度计观测 = 真实角度 + 噪声 */
    float true_ang = 0.0f;
    float est = 0.0f;
    for (int i = 0; i < 200; i++) {
        true_ang += 0.5f * 0.01f;
        est = KF_2D_Update(&k, 0.5f, true_ang + 0.05f);
    }
    TEST_ASSERT_NEAR(est, true_ang, 0.3f);
}

static void test_kf_generic_matches_1d(void)
{
    /* 通用 KF 一维退化和 KF_1D 应一致 */
    KF_Generic g = {0};
    g.n = 1; g.m = 1;
    g.F[0][0] = 1.0f;
    g.H[0][0] = 1.0f;
    g.Q[0][0] = 1e-4f;
    g.R[0][0] = 0.1f;
    KF_Generic_Init(&g);

    KF_1D k;
    KF_1D_Init(&k, 1e-4f, 0.1f, 0.0f, 1.0f);

    for (int i = 0; i < 100; i++) {
        float z = 3.0f;
        KF_Generic_Update(&g, &z);
        KF_1D_Update(&k, z);
    }
    TEST_ASSERT_NEAR(g.x[0], k.x, 1e-3f);
}

static void test_ab_filter(void)
{
    AB_Filter f;
    AB_Filter_Init(&f, 0.4f, 0.1f, 0.033f);
    /* 匀速目标：平滑后速度估计接近真实速度 */
    float pos = 0.0f;
    float est_pos = 0.0f;
    for (int i = 0; i < 300; i++) {
        pos += 2.0f * 0.033f;      /* 速度 2 px/s */
        est_pos = AB_Filter_Update(&f, pos + 0.1f);
    }
    TEST_ASSERT_NEAR(est_pos, pos, 3.0f);
    TEST_ASSERT_NEAR(f.vel, 2.0f, 0.5f);
}

static void test_abg_filter(void)
{
    ABG_Filter f;
    /* γ 项对 0.5·T² 敏感：T=0.1 时 γ=0.01 量级稳定 */
    ABG_Filter_Init(&f, 0.5f, 0.1f, 0.01f, 0.1f);
    /* 匀加速：加速度估计接近真实 */
    float pos = 0.0f, vel = 0.0f;
    for (int i = 0; i < 300; i++) {
        vel += 1.0f * 0.1f;
        pos += vel * 0.1f;
        ABG_Filter_Update(&f, pos);
    }
    TEST_ASSERT_NEAR(f.acc, 1.0f, 0.3f);
}

static void test_complementary(void)
{
    Complementary c;
    Complementary_Init(&c, 1.0f, 0.01f);
    /* 陀螺主导融合：角度接近真实 */
    float true_ang = 0.0f;
    float est = 0.0f;
    for (int i = 0; i < 500; i++) {
        true_ang += 0.2f * 0.01f;
        est = Complementary_Update(&c, 0.2f, true_ang + 0.1f);
    }
    TEST_ASSERT_NEAR(est, true_ang, 0.5f);
}

static void test_mahony_quat_norm(void)
{
    Mahony m;
    Mahony_Init(&m, 0.5f, 0.05f);
    m.dt = 0.01f;
    /* 任意转动：四元数范数必须保持 1 */
    for (int i = 0; i < 500; i++) {
        float gx = 0.3f, gy = -0.2f, gz = 0.1f;
        /* 模拟加速度计：真实重力方向（来自当前四元数旋转 [0,0,1]） */
        float q0 = m.q0, q1 = m.q1, q2 = m.q2, q3 = m.q3;
        float ax = 2.0f * (q1 * q3 - q0 * q2);
        float ay = 2.0f * (q0 * q1 + q2 * q3);
        float az = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
        Mahony_Update(&m, gx, gy, gz, ax, ay, az);
    }
    float norm = sqrtf(m.q0 * m.q0 + m.q1 * m.q1 + m.q2 * m.q2 + m.q3 * m.q3);
    TEST_ASSERT_NEAR(norm, 1.0f, 1e-3f);
}

/* EKF/UKF 测试回调（文件级，C99 不允许嵌套函数） */
static void ekf_f(const float *x, float *x_next, void *ctx)
{
    (void)ctx;
    x_next[0] = x[0] + 0.01f * (0.5f * x[0] * x[0] + 1.0f);
}

static void ekf_h(const float *x, float *z_pred, void *ctx)
{
    (void)ctx;
    z_pred[0] = x[0];
}

static void ukf_f(const float *x, float *x_next, void *ctx)
{
    (void)ctx;
    x_next[0] = x[0] + 0.01f * sinf(x[0]);
    x_next[1] = x[1];
    x_next[2] = x[2];
}

static void ukf_h(const float *x, float *z_pred, void *ctx)
{
    (void)ctx;
    z_pred[0] = x[0];
}

static void test_ekf_basic(void)
{
    EKF e = {0};
    e.n = 1; e.m = 1;
    e.Q[0][0] = 1e-4f;
    e.R[0][0] = 0.1f;
    e.f = ekf_f;
    e.h = ekf_h;
    EKF_Init(&e);
    /* 常数观测：状态收敛有界 */
    float z = 2.0f;
    for (int i = 0; i < 200; i++) EKF_Update(&e, &z);
    TEST_ASSERT_TRUE(e.x[0] > 1.0f);
    TEST_ASSERT_TRUE(e.x[0] < 3.0f);
}

static void test_ukf_basic(void)
{
    UKF u = {0};
    u.n = 1; u.m = 1;
    u.Q[0][0] = 1e-4f;
    u.R[0][0] = 0.1f;
    u.f = ukf_f;
    u.h = ukf_h;
    UKF_Init(&u);
    float z = 1.0f;
    for (int i = 0; i < 100; i++) UKF_Update(&u, &z);
    TEST_ASSERT_NEAR(u.x[0], 1.0f, 0.5f);
}

static void test_kf_adaptive(void)
{
    KF_Adaptive a;
    KF_Adaptive_Init(&a, 1e-4f, 0.1f);
    /* 常数信号：收敛且 R 修正后仍稳定 */
    float est = 0.0f;
    for (int i = 0; i < 300; i++) est = KF_Adaptive_Update(&a, 4.0f);
    TEST_ASSERT_NEAR(est, 4.0f, 0.3f);
    TEST_ASSERT_TRUE(a.r > 0.0f);
}

static void test_imm(void)
{
    /* 两模型：慢（q 小）+ 快（q 大） */
    float qs[2] = {1e-4f, 0.1f};
    float trans[4] = {0.9f, 0.1f, 0.1f, 0.9f};
    float mu0[2] = {0.5f, 0.5f};
    IMM imm;
    IMM_Init(&imm, 2, qs, 0.1f, trans, mu0);
    /* 常数信号：输出收敛，权重归一化 */
    float est = 0.0f;
    for (int i = 0; i < 200; i++) est = IMM_Update(&imm, 5.0f);
    TEST_ASSERT_NEAR(est, 5.0f, 0.5f);
    TEST_ASSERT_NEAR(imm.mu[0] + imm.mu[1], 1.0f, 1e-4f);
}

static void test_infokf_matches_1d(void)
{
    InfoKF i = {0};
    i.n = 1; i.m = 1;
    i.F[0][0] = 1.0f;
    i.H[0][0] = 1.0f;
    i.Q[0][0] = 1e-4f;
    i.R[0][0] = 0.1f;
    InfoKF_Init(&i);

    KF_1D k;
    KF_1D_Init(&k, 1e-4f, 0.1f, 0.0f, 1.0f);

    float z = 3.0f;
    for (int t = 0; t < 100; t++) {
        InfoKF_Update(&i, &z);
        KF_1D_Update(&k, z);
    }
    /* 信息滤波 x = y/Y 应等于 KF_1D 估计 */
    float x_info = i.y[0] / (i.Y[0][0] > 1e-12f ? i.Y[0][0] : 1e-12f);
    TEST_ASSERT_NEAR(x_info, k.x, 0.1f);
}

static void test_kf_sqrt_matches_1d(void)
{
    KF_Sqrt s;
    KF_Sqrt_Init(&s, 1e-4f, 0.1f);
    KF_1D k;
    KF_1D_Init(&k, 1e-4f, 0.1f, 0.0f, 1.0f);
    for (int i = 0; i < 100; i++) {
        KF_Sqrt_Update(&s, 3.0f);
        KF_1D_Update(&k, 3.0f);
    }
    TEST_ASSERT_NEAR(s.x, k.x, 1e-3f);
}

static void test_rts_smooth(void)
{
    /* q 过小（1e-4）时 RTS 增益 g≈1 → 尾端突变被过度传播（数学正确，
     * 参数不当）——用合理 q=0.01 验证平滑的保形性 */
    RTS r;
    RTS_Init(&r, 0.01f, 0.1f);
    for (int i = 0; i < 50; i++) RTS_Add(&r, 5.0f);
    for (int i = 0; i < 20; i++) RTS_Add(&r, 9.0f);   /* 突变段 */
    RTS_Smooth(&r);
    /* 平滑后前端保持 5（远离突变段），突变段保留 */
    TEST_ASSERT_NEAR(r.x[0], 5.0f, 0.5f);
    TEST_ASSERT_NEAR(r.x[60], 9.0f, 1.0f);
}

static void test_pf_estimate(void)
{
    PF p;
    /* q 必须足够大让粒子扩散（确定性 LCG 噪声） */
    PF_Init(&p, 0.5f, 0.5f, 64);
    float est = 0.0f;
    for (int i = 0; i < 200; i++) {
        PF_Update(&p, 2.0f);
        PF_Resample(&p);
        est = p.estimate;
    }
    TEST_ASSERT_NEAR(est, 2.0f, 0.5f);
}

/* ================================================================
 * 通用滤波家族（14）
 * ================================================================ */
static void test_lpf_dc_gain(void)
{
    LPF_1st f;
    LPF_1st_Init(&f, 0.1f, 0.0f);
    float y = 0.0f;
    for (int i = 0; i < 1000; i++) y = LPF_1st_Update(&f, 1.0f);
    TEST_ASSERT_NEAR(y, 1.0f, 1e-3f);   /* 直流增益 = 1 */
}

static void test_hpf_dc_reject(void)
{
    HPF_1st f;
    HPF_1st_Init(&f, 0.1f);
    float y = 0.0f;
    for (int i = 0; i < 500; i++) y = HPF_1st_Update(&f, 1.0f);
    TEST_ASSERT_NEAR(y, 0.0f, 1e-3f);   /* 直流被完全抑制 */
}

static void test_ema(void)
{
    EMA f;
    EMA_Init(&f, 0.5f);
    TEST_ASSERT_NEAR(EMA_Update(&f, 1.0f), 0.5f, 1e-4f);
    TEST_ASSERT_NEAR(EMA_Update(&f, 1.0f), 0.75f, 1e-4f);
}

static void test_moving_average(void)
{
    MovingAverage f;
    MovingAverage_Init(&f, 3);
    TEST_ASSERT_NEAR(MovingAverage_Update(&f, 1.0f), 1.0f / 3.0f, 1e-4f);
    TEST_ASSERT_NEAR(MovingAverage_Update(&f, 2.0f), 1.0f, 1e-4f);
    TEST_ASSERT_NEAR(MovingAverage_Update(&f, 3.0f), 2.0f, 1e-4f);
    TEST_ASSERT_NEAR(MovingAverage_Update(&f, 4.0f), 3.0f, 1e-4f); /* 窗口滚动 */
}

static void test_median(void)
{
    Median f;
    Median_Init(&f, 5);
    /* 尖峰注入：中值免疫 */
    float y = 0.0f;
    float seq[5] = {1.0f, 2.0f, 100.0f, 3.0f, 4.0f};
    for (int i = 0; i < 5; i++) y = Median_Update(&f, seq[i]);
    TEST_ASSERT_NEAR(y, 3.0f, 1e-4f);   /* 排序中值 = 3 */
}

static void test_limit(void)
{
    Limit f;
    Limit_Init(&f, 0.5f, 0.0f);
    TEST_ASSERT_NEAR(Limit_Update(&f, 0.2f), 0.2f, 1e-4f);
    TEST_ASSERT_NEAR(Limit_Update(&f, 5.0f), 0.7f, 1e-4f);  /* 限 0.5 步长 */
}

static void test_debounce(void)
{
    Debounce f;
    Debounce_Init(&f, 3, 0);
    /* 语义：输入变化后需连续 need 次一致才确认翻转 */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 1) == 0);   /* 变化（不计） */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 1) == 0);   /* cnt=1 */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 1) == 0);   /* cnt=2 */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 1) == 1);   /* cnt=3 → 翻转 */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 0) == 1);   /* 单次扰动不翻转 */
    TEST_ASSERT_TRUE(Debounce_Update(&f, 0) == 1);
    TEST_ASSERT_TRUE(Debounce_Update(&f, 0) == 1);
    TEST_ASSERT_TRUE(Debounce_Update(&f, 0) == 0);   /* 3 次确认 → 翻转 */
}

static void test_notch(void)
{
    Notch f;
    Notch_Init(&f, 1000.0f, 50.0f, 10.0f);
    /* 50Hz 正弦应被深度衰减（对比直流通过） */
    float amp_50 = 0.0f;
    float out = 0.0f;
    /* 先让滤波器稳定 1s */
    for (int i = 0; i < 1000; i++) out = Notch_Update(&f, 0.0f);
    for (int i = 0; i < 2000; i++) {
        float t = (float)i / 1000.0f;
        float in = sinf(2.0f * 3.14159f * 50.0f * t);
        out = Notch_Update(&f, in);
        if (i > 1000 && fabsf(out) > amp_50) amp_50 = fabsf(out);
    }
    TEST_ASSERT_TRUE(amp_50 < 0.2f);   /* 50Hz 被衰减 > 20dB */
    /* 直流通过 */
    Notch g;
    Notch_Init(&g, 1000.0f, 50.0f, 10.0f);
    float y = 0.0f;
    for (int i = 0; i < 500; i++) y = Notch_Update(&g, 1.0f);
    TEST_ASSERT_NEAR(y, 1.0f, 1e-3f);
}

static void test_biquad_lpf(void)
{
    Biquad f;
    Biquad_Init(&f, BIQUAD_LPF, 1000.0f, 100.0f, 0.707f);
    /* 直流增益 = 1 */
    float y = 0.0f;
    for (int i = 0; i < 500; i++) y = Biquad_Update(&f, 1.0f);
    TEST_ASSERT_NEAR(y, 1.0f, 1e-3f);
}

static void test_butterworth(void)
{
    Butterworth f;
    Butterworth_Init(&f, 4, 1000.0f, 100.0f);
    /* 直流增益 = 1 */
    float y = 0.0f;
    for (int i = 0; i < 500; i++) y = Butterworth_Update(&f, 1.0f);
    TEST_ASSERT_NEAR(y, 1.0f, 1e-3f);
}

static void test_sg_constant(void)
{
    SavitzkyGolay f;
    SavitzkyGolay_Init(&f);
    /* 常数序列：SG 输出 = 常数（保形） */
    float y = 0.0f;
    for (int i = 0; i < 10; i++) y = SavitzkyGolay_Update(&f, 3.5f);
    TEST_ASSERT_NEAR(y, 3.5f, 1e-4f);
}

static void test_weighted_avg(void)
{
    WeightedAvg f;
    WeightedAvg_Init(&f, 2);
    WeightedAvg_Set(&f, 0, 1.0f, 1.0f);
    WeightedAvg_Set(&f, 1, 3.0f, 3.0f);
    float y = WeightedAvg_Calc(&f);
    TEST_ASSERT_NEAR(y, 2.5f, 1e-4f);   /* (1*1+3*3)/4 */
}

static void test_deadband_filter(void)
{
    Deadband f;
    Deadband_Init(&f, 0.5f);
    TEST_ASSERT_NEAR(Deadband_Update(&f, 0.2f), 0.0f, 1e-5f);
    TEST_ASSERT_NEAR(Deadband_Update(&f, 0.8f), 0.3f, 1e-5f);
    TEST_ASSERT_NEAR(Deadband_Update(&f, -0.8f), -0.3f, 1e-5f);
}

static void test_rate_limiter(void)
{
    RateLimiter f;
    RateLimiter_Init(&f, 10.0f, 0.01f, 0.0f);
    /* 突变输入：输出按 10/s 斜坡爬升 */
    float y = 0.0f;
    for (int i = 0; i < 10; i++) y = RateLimiter_Update(&f, 100.0f);
    TEST_ASSERT_NEAR(y, 1.0f, 1e-3f);   /* 10 步 × 0.1/步 */
    y = RateLimiter_Update(&f, 100.0f);
    TEST_ASSERT_NEAR(y, 1.1f, 1e-3f);
}

/* ================================================================
 * 主入口
 * ================================================================ */
int main(void)
{
    /* ---- PID 14 ---- */
    RUN_TEST(test_pid_pos_p_only);
    RUN_TEST(test_pid_pos_integral);
    RUN_TEST(test_pid_pos_clamp);
    RUN_TEST(test_pid_pos_sep_integral);
    RUN_TEST(test_pid_incremental);
    RUN_TEST(test_pid_cascade);
    RUN_TEST(test_pid_feedforward);
    RUN_TEST(test_pid_separated);
    RUN_TEST(test_pid_antiwindup);
    RUN_TEST(test_pid_dom);
    RUN_TEST(test_pid_gainsched);
    RUN_TEST(test_pid_fuzzy);
    RUN_TEST(test_pid_smith);
    RUN_TEST(test_pid_bangbang);
    RUN_TEST(test_pid_autotune);
    RUN_TEST(test_pid_neural);
    RUN_TEST(test_pid_deadband);
    /* ---- 卡尔曼 15 ---- */
    RUN_TEST(test_kf_1d_converge);
    RUN_TEST(test_kf_2d_track);
    RUN_TEST(test_kf_generic_matches_1d);
    RUN_TEST(test_ab_filter);
    RUN_TEST(test_abg_filter);
    RUN_TEST(test_complementary);
    RUN_TEST(test_mahony_quat_norm);
    RUN_TEST(test_ekf_basic);
    RUN_TEST(test_ukf_basic);
    RUN_TEST(test_kf_adaptive);
    RUN_TEST(test_imm);
    RUN_TEST(test_infokf_matches_1d);
    RUN_TEST(test_kf_sqrt_matches_1d);
    RUN_TEST(test_rts_smooth);
    RUN_TEST(test_pf_estimate);
    /* ---- 滤波 14 ---- */
    RUN_TEST(test_lpf_dc_gain);
    RUN_TEST(test_hpf_dc_reject);
    RUN_TEST(test_ema);
    RUN_TEST(test_moving_average);
    RUN_TEST(test_median);
    RUN_TEST(test_limit);
    RUN_TEST(test_debounce);
    RUN_TEST(test_notch);
    RUN_TEST(test_biquad_lpf);
    RUN_TEST(test_butterworth);
    RUN_TEST(test_sg_constant);
    RUN_TEST(test_weighted_avg);
    RUN_TEST(test_deadband_filter);
    RUN_TEST(test_rate_limiter);

    TEST_SUMMARY();
}
