/* ================================================================
 * xl9555.h —— XL9555 16 位 IO 扩展芯片驱动（I2C0，地址 0x20）
 *
 * 板上它承担了 8 个"零散控制线"：
 *   背光、蜂鸣器、功放使能、触摸复位、红色 LED、两个传感器中断输入、
 *   以及 6 路扩展排针 IO —— 全部经由这一颗芯片。
 *
 * 性能/健壮性设计：
 *   1. 影子寄存器 + 掩码写：改一位只发一次 I2C 事务（3 字节，同时更新 P0/P1），
 *      不做"读-改-写"，也不为一次改动发两次事务；
 *   2. 内部互斥锁保证多任务并发改不同引脚时不会互相丢失更新；
 *   3. 中断用 IO3 下降沿 + 二值信号量，中断服务函数里不做 I2C 访问，
 *      由调用任务唤醒后再读输入寄存器（顺带清中断）。
 * ================================================================ */
#ifndef __BSP_XL9555_H
#define __BSP_XL9555_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 生命周期 ---------------- */

/**
 * @brief  初始化 XL9555（幂等）
 * @note   会顺带安装 GPIO ISR 服务（板上按键/中断共用），因此它是
 *         LED/BEEP/LCD/TOUCH/KEY 等模块的公共依赖。
 */
esp_err_t bsp_xl9555_init(void);

/**
 * @brief  反初始化（复位引脚方向为输入，避免残留输出)
 */
esp_err_t bsp_xl9555_deinit(void);

/* ---------------- 输出 ---------------- */

/**
 * @brief  按掩码写输出（推荐：一次事务改多个引脚）
 * @param  mask   要修改的位（1 = 修改）
 * @param  value  对应位的目标电平
 *
 * @note   输出寄存器是"电平"语义；上电缺省值见 xl9555.c 的 OUT_*_INIT。
 */
esp_err_t bsp_xl9555_write_mask(uint16_t mask, uint16_t value);

/**
 * @brief  写单个输出引脚
 */
esp_err_t bsp_xl9555_pin_write(uint16_t pin, bool level);

/**
 * @brief  读取单个输入引脚电平（输入寄存器，实时值）
 */
bool bsp_xl9555_pin_read(uint16_t pin);

/**
 * @brief  读取 16 位输入寄存器（会顺带清除中断）
 */
esp_err_t bsp_xl9555_inputs(uint16_t *out);

/**
 * @brief  获取输出影子寄存器（不产生 I2C 事务）
 */
uint16_t bsp_xl9555_outputs(void);

/* ---------------- 中断（IO3）---------------- */

/**
 * @brief  中断回调原型
 * @note   **在中断上下文执行**：只能做 ISR 安全的事情
 *         （例如 vTaskNotifyGiveFromISR / xQueueSendFromISR /
 *          xSemaphoreGiveFromISR），绝对不能做 I2C 访问或阻塞调用。
 */
typedef void (*bsp_xl9555_int_cb_t)(void *ctx);

/**
 * @brief  注册中断回调（可选，与 bsp_xl9555_int_wait 可同时使用）
 */
esp_err_t bsp_xl9555_attach_int(bsp_xl9555_int_cb_t cb, void *ctx);

/**
 * @brief  注销中断回调
 */
esp_err_t bsp_xl9555_detach_int(void);

/**
 * @brief  使能/关闭 XL9555 中断输入
 * @param  enable true = 允许中断（下降沿），false = 关闭
 * @note   XL9555 的中断在"任一输入电平变化"时拉低，读取输入寄存器后释放。
 */
esp_err_t bsp_xl9555_int_enable(bool enable);

/**
 * @brief  等待一次中断事件（任务上下文调用）
 * @param  timeout_ms 超时时间
 * @return ESP_OK 有事件；ESP_ERR_TIMEOUT 超时
 *
 * @note   返回后建议立即 bsp_xl9555_inputs() 读一次输入寄存器：
 *         既拿到最新电平，又清掉中断源。
 */
esp_err_t bsp_xl9555_int_wait(uint32_t timeout_ms);

/**
 * @brief  清中断（读一次输入寄存器）
 */
esp_err_t bsp_xl9555_int_clear(void);

/**
 * @brief  丢弃已积累的中断事件（重连/恢复时用）
 */
void bsp_xl9555_int_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_XL9555_H */
