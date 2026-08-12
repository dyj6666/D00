/* ================================================================
 * bsp_can —— CAN1 板级驱动：1Mbps 极限速率 + 双 FIFO 中断 + 邮箱并发 TX
 *
 * 架构位置：APP BSP 层；服务层（shell/OTA）只经本层访问 CAN 硬件
 *
 * 性能设计：
 *   - 位定时 1Mbps（bxCAN 上限），采样点 76%：APB1=42MHz，预分频 2，
 *     BS1=15TQ + BS2=5TQ（1+15+5=21TQ → 1Mbps）；
 *   - RX：FIFO0/FIFO1 双缓冲全收（掩码 0），中断入队 → canRx 任务分发，
 *     ISR 内零业务逻辑，突发不丢帧；
 *   - TX：3 邮箱 + canTx 任务批量填充，ABOM 硬件自动退出 bus-off，
 *     NART=0 自动重发保证总线竞争不丢帧；
 *   - 统计：收发计数/错误等级/总线占用率，供 `can status` 与 LCD 巡检。
 *
 * 硬件：探索者V3 板载 CAN1（PA11=RX PA12=TX，TJA1050 收发器）；
 * P5 跳线帽须拨到 CAN 侧（勿与 USB OTG 共用）。
 * ================================================================ */
#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>

/* CAN 标准帧数据场上限（硬件固定） */
#define BSP_CAN_DLC_MAX  8u
/* 内部队列深度与消费者注册表容量 */
#define BSP_CAN_RX_QUEUE_LEN  64u
#define BSP_CAN_TX_QUEUE_LEN  32u
#define BSP_CAN_MAX_CB        4u

/* CAN 工作模式（可运行中切换，loopback 用于无总线自测） */
typedef enum {
    BSP_CAN_MODE_NORMAL = 0,          /* 正常总线模式 */
    BSP_CAN_MODE_LOOPBACK,            /* 内部回环：发即收，不占总线 */
    BSP_CAN_MODE_SILENT_LOOPBACK,     /* 静默回环：只收不发 */
} bsp_can_mode_t;

/* 运行统计（`can status` / LCD 巡检读取） */
typedef struct {
    uint32_t tx_ok;            /* 成功入邮箱帧数 */
    uint32_t tx_err;           /* 发送失败帧数（队列满/邮箱异常） */
    uint32_t rx_ok;            /* 已分发给注册回调的帧数 */
    uint32_t rx_other;         /* 无消费者帧数（总线嗅探观察量） */
    uint32_t rx_drop;          /* ISR 入队失败丢帧 */
    uint32_t rx_overrun;       /* FIFO 溢出次数 */
    uint32_t err_warning;      /* 错误警告（EWG）进入次数 */
    uint32_t err_passive;      /* 错误被动（EPV）进入次数 */
    uint32_t err_busoff;       /* bus-off 进入次数（ABOM 自动恢复） */
    uint32_t last_error;       /* 最近一次 HAL CAN 错误码 */
    uint32_t bus_load_permille;/* 近窗口总线占用率（‰，千分比） */
} bsp_can_stats_t;

/* RX 帧回调（canRx 任务上下文，可安全调用 FreeRTOS/命令框架） */
typedef void (*bsp_can_rx_cb_t)(uint32_t id, const uint8_t *data,
                                uint8_t dlc, void *ctx);

/** @brief 初始化 CAN1：时钟/引脚/位定时/滤波/中断/队列/收发任务 */
void BSP_CAN_Init(void);

/** @brief 运行中切换工作模式（loopback 自测 / 恢复总线） */
void BSP_CAN_SetMode(bsp_can_mode_t mode);

/** @brief 查询当前工作模式 */
bsp_can_mode_t BSP_CAN_GetMode(void);

/** @brief 发送一帧（标准帧，≤8B）：入 TX 队列，异步由 canTx 任务发送 */
int BSP_CAN_Send(uint32_t id, const uint8_t *data, uint8_t dlc);

/** @brief 注册 RX 帧回调（任务上下文调用；最多 BSP_CAN_MAX_CB 个） */
int BSP_CAN_RegisterRxCb(bsp_can_rx_cb_t cb, void *ctx);

/** @brief 取统计快照（含近窗口总线占用率） */
void BSP_CAN_GetStats(bsp_can_stats_t *st);

/** @brief 清零统计计数 */
void BSP_CAN_ResetStats(void);

/** @brief 总线是否可用（已启动且非 bus-off） */
int BSP_CAN_IsActive(void);

/** @brief 读硬件错误计数 TEC/REC（非空指针才回填） */
uint32_t BSP_CAN_GetErrorCounters(uint8_t *tec, uint8_t *rec);

/** @brief 供 stm32f4xx_it.c 三个 CAN1 中断统一入口（内部调 HAL_CAN_IRQHandler） */
void BSP_CAN_IRQHandler(void);

#endif /* BSP_CAN_H */
