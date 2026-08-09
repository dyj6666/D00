#ifndef I2C_H
#define I2C_H

#include "stm32f4xx_hal.h"

/* MPU6050 + 板载 AT24C02（I2C1，PB6=SCL / PB7=SDA，400kHz） */
extern I2C_HandleTypeDef hi2c1;
/* 备用 I2C2（PB10=SCL / PB11=SDA，100kHz）：EEPROM 探测/外设扩展 */
extern I2C_HandleTypeDef hi2c2;

void MX_I2C1_Init(void);
void MX_I2C2_Init(void);

#endif
