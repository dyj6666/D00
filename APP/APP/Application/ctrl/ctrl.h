/* ================================================================
 * ctrl.h —— 控制与滤波算法库统一头
 *
 * 架构位置：APP 应用层 ctrl/ 子模块；纯 C、无 HAL/RTOS 依赖，
 *           可主机单元测试。全部算法实例化 = 结构体（可重入、多实例）。
 *
 * 包含：
 *   pid.h      —— PID 全家族 14 变式
 *   kalman.h   —— 卡尔曼/贝叶斯全家族 15 变式
 *   filter.h   —— 通用信号滤波 14 种
 *
 * 用法：业务模块只需 #include "ctrl/ctrl.h" 即获得全部算法。
 * ================================================================ */
#ifndef CTRL_H
#define CTRL_H

#include "pid.h"
#include "kalman.h"
#include "filter.h"

/* 算法库版本（随固件演进） */
#define CTRL_LIB_VERSION   "1.0.0"

#endif /* CTRL_H */
