/* 上位机变量 ID 集中分配表
 *
 * 规则：新增变量必须先在 __这里__ 登记一个唯一 ID，再在模块中引用，
 * 禁止在模块内随手写数字，避免 ID 冲突。
 */
/* ================================================================
 * var_ids —— 变量 ID 集中分配表
 *
 * 架构位置：APP 配置层；HOSTLINK 变量契约
 * ================================================================ */
#ifndef VAR_IDS_H
#define VAR_IDS_H

/* 0x1xxx: LedApp */
#define VAR_ID_LED_STATE    0x1001
#define VAR_ID_WRITABLE     0x2001

/* 0x3xxx: DataAgent */
#define VAR_ID_SYS_TICK     0x3001

/* 0x7xxx: LogicAnalyzer */
#define VAR_ID_LA_SAMPLES   0x7001
#define VAR_ID_LA_CH0       0x7002
#define VAR_ID_LA_CH3       0x7003

/* 0x8xxx: EthApp */
#define VAR_ID_ETH_LINK     0x8001
#define VAR_ID_ETH_RX       0x8002
#define VAR_ID_ETH_TX       0x8003

#endif
