/* ================================================================
 * bsp_flash —— 片内 Flash 擦写封装（扇区擦除 / 字编程 / 任意对齐写）
 *
 * 架构位置：APP BSP 层；唯一允许直接操作 Flash 控制器的接口。
 * 调用方（OTA/参数持久化等）通过本模块访问 Flash，禁止直接使用 HAL_FLASH。
 * ================================================================ */
#ifndef BSP_FLASH_H
#define BSP_FLASH_H

#include <stdint.h>
#include <stdbool.h>

/** @brief 擦除 [addr, addr+len) 覆盖的全部扇区
 *  @param addr 起始地址（Flash 绝对地址）
 *  @param len  长度；0 表示仅擦 addr 所在扇区
 *  @return true=全部扇区擦除成功 */
bool BSP_Flash_EraseRange(uint32_t addr, uint32_t len);

/** @brief 单字 Flash 编程（逐字关中断，保证 ISR/OTA 并发安全）
 *  @param addr 目标地址（须 4 字节对齐）
 *  @param val  要写入的 32 位字
 *  @return true=编程成功 */
bool BSP_Flash_ProgramWord(uint32_t addr, uint32_t val);

/** @brief 向 Flash 写入任意长度数据（自动处理字对齐）
 *  @param addr 目标地址（任意字节对齐）
 *  @param data 源数据指针
 *  @param len  字节数
 *  @return true=全部写入成功 */
bool BSP_Flash_Write(uint32_t addr, const uint8_t *data, uint32_t len);

/** @brief 复位 Flash 控制器状态（清错误标志 + 解锁/锁定往返）
 *  @note  断点续传等场景在擦写前调用，避免残留错误标志导致编程 BSY 卡死 */
void BSP_Flash_ResetController(void);

/** @brief 读取 Flash 状态寄存器 SR（写失败诊断用） */
uint32_t BSP_Flash_GetStatusSR(void);

#endif /* BSP_FLASH_H */
