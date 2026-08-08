#ifndef I2C_H
#define I2C_H

#include "stm32f4xx_hal.h"

/* 外接 MPU6050 专用 I2C1（PB6=SCL / PB7=SDA，400kHz） */
extern I2C_HandleTypeDef hi2c1;

void MX_I2C1_Init(void);

#endif
