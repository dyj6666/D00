/* ================================================================
 * key.h —— 按键驱动（BOOT + KEY0 + KEY1）
 *
 * 硬件：
 *   BOOT(IO0)      : 直接接 GPIO，低有效，带外部上拉；
 *   KEY0/KEY1      : 接 XL9555 的 P0_4 / P0_3（低有效），
 *                    变化通过 XL9555 的 INT(IO3) 汇总通知。
 *
 * 性能设计（不轮询）：
 *   常态下任务阻塞在任务通知上，CPU 占用为零；
 *   只有"确实有键按下"期间才以 10ms 节拍扫描，用于消抖与长按计时；
 *   松手后立刻回到阻塞态。
 *
 * 事件语义：
 *   DOWN  -> 消抖确认按下
 *   UP    -> 消抖确认松开
 *   CLICK -> 按下时长 < long_press_ms 的"短按"（在 UP 之后发出）
 *   LONG_PRESS -> 按住达到 long_press_ms（只发一次）
 *
 * 回调在按键任务上下文执行，**允许阻塞**（但不要在里面做耗时操作）。
 * ================================================================ */
#ifndef __BSP_KEY_H
#define __BSP_KEY_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 按键编号 */
typedef enum
{
    BSP_KEY_BOOT = 0,       /*!< IO0 上的 BOOT 键 */
    BSP_KEY_0,              /*!< 丝印 K1，经 XL9555 P0_4 */
    BSP_KEY_1,              /*!< 丝印 K2，经 XL9555 P0_3 */
    BSP_KEY_COUNT
} bsp_key_id_t;

/** 按键事件 */
typedef enum
{
    BSP_KEY_EVENT_DOWN = 0,
    BSP_KEY_EVENT_UP,
    BSP_KEY_EVENT_CLICK,
    BSP_KEY_EVENT_LONG_PRESS,
} bsp_key_event_t;

/**
 * @brief  按键事件回调
 * @param  key      哪个键
 * @param  event    事件类型
 * @param  hold_ms  事件发生时的已按住时长（UP/CLICK/LONG_PRESS 有意义）
 * @param  ctx      用户上下文
 */
typedef void (*bsp_key_cb_t)(bsp_key_id_t key, bsp_key_event_t event, uint32_t hold_ms, void *ctx);

/** 按键配置（可选用 bsp_key_configure 覆盖默认值） */
typedef struct
{
    uint32_t debounce_ms;       /*!< 消抖时间，默认 20ms */
    uint32_t long_press_ms;     /*!< 长按门限，默认 1000ms */
    uint32_t poll_period_ms;    /*!< 按下期间的扫描节拍，默认 10ms */
} bsp_key_cfg_t;

/** 默认配置：20ms 消抖 / 1000ms 长按 / 10ms 扫描 */
#define BSP_KEY_CFG_DEFAULT     { 20U, 1000U, 10U }

/**
 * @brief  覆盖默认配置（必须在 bsp_key_init 之前调用）
 */
esp_err_t bsp_key_configure(const bsp_key_cfg_t *cfg);

/**
 * @brief  注册事件回调（可在 init 前后调用）
 */
esp_err_t bsp_key_set_callback(bsp_key_cb_t cb, void *ctx);

/**
 * @brief  初始化按键（幂等）：配置 GPIO、XL9555 中断、启动扫描任务
 */
esp_err_t bsp_key_init(void);

/**
 * @brief  反初始化：停止任务、关闭中断
 */
esp_err_t bsp_key_deinit(void);

/**
 * @brief  查询某个键当前是否处于按下状态（已消抖）
 */
bool bsp_key_is_down(bsp_key_id_t key);

/**
 * @brief  查询某个键已按住的时长（毫秒，未按下返回 0）
 */
uint32_t bsp_key_hold_ms(bsp_key_id_t key);

/**
 * @brief  一次性读取某个键的状态
 */
esp_err_t bsp_key_get(bsp_key_id_t key, bool *down, uint32_t *hold_ms);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_KEY_H */
