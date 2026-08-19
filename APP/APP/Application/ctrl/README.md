# ctrl/ —— 控制与滤波算法库（43 算法全家族）

纯 C、float（FPU 加速）、无 HAL/RTOS 依赖、结构体实例化（可重入/多实例/无动态内存）。
业务模块 `#include "ctrl/ctrl.h"` 即获得全部算法。

## 文件结构

```
ctrl/
├── ctrl.h      统一入口（include pid/kalman/filter）
├── pid.h/c     PID 全家族 14 变式
├── kalman.h/c  卡尔曼/贝叶斯全家族 15 变式
└── filter.h/c  通用信号滤波 14 种
```

## PID 家族（pid.h/c）—— 14 变式

| # | 变式 | 一句话 | 典型场景 |
|---|---|---|---|
| 01 | `PID_Pos` | 位置式（全特性：积分分离/微分低通/限幅） | 温度/压力/位置慢回路 |
| 02 | `PID_Incremental` | 增量式（输出=Δu） | 舵机/阀门防冲击 |
| 03 | `PID_Cascade` | 串级（位置环×速度环） | 云台/平衡车/四旋翼标准架构 |
| 04 | `PID_FeedForward` | 前馈补偿 | 视觉目标速度前馈/重力补偿 |
| 05 | `PID_Separated` | 积分分离 | 大阶跃防超调 |
| 06 | `PID_AntiWindup` | Back-Calculation 抗饱和 | 执行器饱和回路 |
| 07 | `PID_DerivativeOnMeasure` | 微分先行 | 目标阶跃+噪声传感器 |
| 08 | `PID_GainSched` | 增益调度（分段插值） | 非线性对象分段线性化 |
| 09 | `PID_Fuzzy` | 模糊规则修正增益（重心法） | 难建模对象 |
| 10 | `PID_Smith` | 史密斯预估（一阶模型+延迟缓冲） | 视觉 30-100ms 延迟 |
| 11 | `PID_BangBang` | 粗调+精调混合 | 快速捕获目标 |
| 12 | `PID_Autotune` | 继电反馈自整定（Z-N 公式） | 一键自动整定 |
| 13 | `PID_Neural` | 单神经元 Hebb 学习 | 参数慢变自学习 |
| 14 | `PID_Deadband` | 死区+输出限速 | 机械回差/执行器保护 |

## 卡尔曼/贝叶斯家族（kalman.h/c）—— 15 变式

| # | 变式 | 一句话 | 典型场景 |
|---|---|---|---|
| 01 | `KF_Generic` | 通用多维卡尔曼（≤4 维矩阵） | 线性状态估计教学基准 |
| 02 | `KF_1D` | 一维标量卡尔曼 | 单传感器平滑入门 |
| 03 | `KF_2D` | 角度+零偏（IMU 经典） | MPU6050 姿态 |
| 04 | `AB_Filter` | α-β 滤波（KF 特例） | 视觉目标位置/速度平滑 |
| 05 | `ABG_Filter` | α-β-γ（恒定加速度） | 高机动目标 |
| 06 | `Complementary` | 互补滤波（τ 时间常数） | 姿态务实基线 |
| 07 | `Mahony` | 四元数 AHRS 互补 | 全姿态 roll/pitch/yaw |
| 08 | `EKF` | 扩展卡尔曼（数值差分雅可比） | 非线性状态估计 |
| 09 | `UKF` | 无迹卡尔曼（sigma 点） | 强非线性 |
| 10 | `KF_Adaptive` | 自适应（创新方差匹配 R） | 噪声时变环境 |
| 11 | `IMM` | 交互式多模型（2-3 模型加权） | 机动目标跟踪 |
| 12 | `InfoKF` | 信息滤波（协方差逆） | 多传感器融合教学 |
| 13 | `KF_Sqrt` | 平方根卡尔曼（数值稳定） | 长时间运行/病态场景 |
| 14 | `RTS` | RTS 平滑（离线后向校正） | 轨迹后处理/对比基准 |
| 15 | `PF` | 粒子滤波（系统重采样） | 非高斯/多峰分布 |

## 通用滤波家族（filter.h/c）—— 14 种

| # | 滤波 | 一句话 | 典型场景 |
|---|---|---|---|
| 01 | `LPF_1st` | 一阶低通（RC） | 传感器噪声平滑 |
| 02 | `HPF_1st` | 一阶高通 | 去直流/漂移 |
| 03 | `EMA` | 指数移动平均 | 轻量平滑 |
| 04 | `MovingAverage` | 滑动平均（N 点） | 保形平滑 |
| 05 | `Median` | 中值（窗内排序） | 脉冲/椒盐噪声 |
| 06 | `Limit` | 限幅（相邻差限幅） | 粗差剔除 |
| 07 | `Debounce` | 消抖（连续 N 次确认） | 按键/开关 |
| 08 | `Notch` | 陷波（IIR 带阻） | 50Hz 工频 |
| 09 | `Biquad` | 双二阶（LP/HP/BP/BS，RBJ） | 任意 IIR 积木 |
| 10 | `Butterworth` | 巴特沃斯低通（级联 2-8 阶） | 精密测量/抗混叠 |
| 11 | `SavitzkyGolay` | SG 平滑（5 点 2 次） | 曲线保形 |
| 12 | `WeightedAvg` | 信任度加权融合 | 多传感器 |
| 13 | `Deadband` | 死区 | 零位校准/静区 |
| 14 | `RateLimiter` | 变化率限制（斜坡） | 执行器/指令保护 |

## 设计约定

- **实例化**：每个算法一个结构体，调用方持有实例（可多个并存）；
- **无动态内存**：全部固定数组（窗口/矩阵上限见各头文件宏）；
- **数值守卫**：除法/开方带零值保护；协方差对称化；
- **纯计算**：不访问外设、不依赖 tick——采样周期 dt 由调用方注入；
- **单元测试友好**：可在主机 gcc 直接编译验证。

## 快速示例（云台角度环 + 视觉目标平滑）

```c
#include "ctrl/ctrl.h"

/* 1. 视觉目标平滑（α-β） */
AB_Filter target = {0};
AB_Filter_Init(&target, 0.4f, 0.1f, 0.033f);   /* 30Hz 视觉 */
float x_smooth = AB_Filter_Update(&target, openart_x);

/* 2. 云台角度串级控制 */
PID_Cascade gimbal = {0};
gimbal.outer.kp = 2.0f; gimbal.outer.ki = 0.5f; gimbal.outer.dt = 0.001f;
gimbal.inner.kp = 0.1f; gimbal.inner.ki = 0.02f; gimbal.inner.dt = 0.001f;
PID_Cascade_Init(&gimbal);
PID_Cascade_SetInnerMeas(&gimbal, gyro_rate);
float pwm = PID_Cascade_Update(&gimbal, angle_ref - angle_meas);
```

## 验证状态

- arm-none-eabi-gcc -Wall -Wextra：3 模块全部 0 错误 0 警告
- 数值正确性单测（Unity）规划中——见 docs/UNIT_TESTING_PLAN.md
