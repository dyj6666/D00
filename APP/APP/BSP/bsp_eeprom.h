#ifndef BSP_EEPROM_H
#define BSP_EEPROM_H

#include <stdint.h>

/* ================================================================
 * 板载 AT24C02 EEPROM（I2C2：PB10=SCL / PB11=SDA，7 位地址 0x50）
 *   - 256B，8B 页写；EEPROM 支持任意字节就地改写（非 flash 只清位）
 *   - 与 MPU6050（I2C1/PB6-PB7）分属两总线；内部经 BSP_I2C2 互斥
 *   - 注意：PB8/PB9 为 CAN1 引脚（探索者 V3），不可复用为 I2C
 * ================================================================ */

#define BSP_EEPROM_ADDR     (0x50u << 1)
#define BSP_EEPROM_SIZE     256
#define BSP_EEPROM_PAGE     8

/* 探测器件（总线应答）；0=在线 */
int BSP_EEPROM_Probe(void);

/* 初始化（建总线锁 + 探测）；0=成功 */
int BSP_EEPROM_Init(void);

/* 随机读 / 页感知写；0=成功，负=参数/总线错误 */
int BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len);
int BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len);

#endif
