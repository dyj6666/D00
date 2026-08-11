/* ================================================================
 * bsp_i2c —— I2C 总线抽象：寄存器读写 + 超时保护
 *
 * 架构位置：APP BSP 层；EEPROM/MPU6050 挂载于此
 * ================================================================ */
#ifndef BSP_I2C_H
#define BSP_I2C_H

#include <stdint.h>

/* ================================================================
 * I2C1（PB6/PB7）/ I2C2（PB10/PB11）总线互斥：
 * 所有 HAL I2C 事务必须包在对应 Lock/Unlock 之间（HAL 非线程安全）。
 * ================================================================ */

void BSP_I2C1_Init(void);                  /* 创建互斥（幂等） */
int  BSP_I2C1_Lock(uint32_t timeout_ms);   /* 0=成功，-1=超时/未初始化 */
void BSP_I2C1_Unlock(void);

void BSP_I2C2_Init(void);
int  BSP_I2C2_Lock(uint32_t timeout_ms);
void BSP_I2C2_Unlock(void);

#endif
