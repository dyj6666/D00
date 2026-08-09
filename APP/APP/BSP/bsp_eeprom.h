#ifndef BSP_EEPROM_H
#define BSP_EEPROM_H

#include <stdint.h>

/* ================================================================
 * 板载 AT24C02 EEPROM（软件模拟 IIC：PB8=SCL / PB9=SDA，7 位地址 0x50）
 *   - 256B，8B 页写；EEPROM 支持任意字节就地改写（非 flash 只清位）
 *   - 官方（正点原子探索者 F407）驱动方式：GPIO 位操作软 IIC，
 *     与 MPU6050（硬件 I2C1/PB6-PB7）完全独立，互不干扰
 *   - 内部互斥串行化；读写前请确保 BSP_EEPROM_Init 已调用
 * ================================================================ */

#define BSP_EEPROM_ADDR     (0x50u << 1)
#define BSP_EEPROM_SIZE     256
#define BSP_EEPROM_PAGE     8

/* 探测器件（总线应答）；0=在线 */
int BSP_EEPROM_Probe(void);

/* 初始化（GPIO + DWT 延时 + 探测）；0=成功 */
int BSP_EEPROM_Init(void);

/* 随机读 / 页感知写；0=成功，负=参数/总线错误 */
int BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len);
int BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len);

#endif
