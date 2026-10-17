/* ================================================================
 * eeprom.h —— 板载 AT24C02 EEPROM（256 字节，I2C 0x50）
 *
 * 用途：保存掉电不能丢的小数据（校准值、配置、开机计数…）。
 * 特性：
 *   - 页写（8 字节/页）自动切分，调用方按任意长度写入即可；
 *   - 写周期用 **ACK 轮询**（bsp_i2c_probe）代替固定 5ms 延时，
 *     连续写多页时总耗时比"每页死等 5ms"快 2~3 倍；
 *   - 内部加锁，保证跨页写不被其它 I2C 事务打断。
 * ================================================================ */
#ifndef __BSP_EEPROM_H
#define __BSP_EEPROM_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_EEPROM_SIZE         256U    /*!< AT24C02 容量（字节） */
#define BSP_EEPROM_PAGE_SIZE    8U      /*!< 页大小（字节） */

/**
 * @brief  初始化（幂等）：挂载 I2C 设备并探测器件
 */
esp_err_t bsp_eeprom_init(void);

/**
 * @brief  反初始化
 */
esp_err_t bsp_eeprom_deinit(void);

/**
 * @brief  读任意长度
 * @param  addr 起始地址 0~(BSP_EEPROM_SIZE-1)
 */
esp_err_t bsp_eeprom_read(uint16_t addr, void *buf, size_t len);

/**
 * @brief  写任意长度（自动跨页 + 等待写周期结束）
 */
esp_err_t bsp_eeprom_write(uint16_t addr, const void *data, size_t len);

/**
 * @brief  读 1 字节
 */
esp_err_t bsp_eeprom_read_u8(uint16_t addr, uint8_t *val);

/**
 * @brief  写 1 字节
 */
esp_err_t bsp_eeprom_write_u8(uint16_t addr, uint8_t val);

/**
 * @brief  自检：在指定地址写入测试图案再读回校验，最后恢复原值
 * @param  addr 用于测试的地址（建议用最后几个字节，避开业务数据）
 * @return ESP_OK 通过
 */
esp_err_t bsp_eeprom_self_test(uint16_t addr);

/**
 * @brief  擦除（整片写 0xFF）
 */
esp_err_t bsp_eeprom_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_EEPROM_H */
