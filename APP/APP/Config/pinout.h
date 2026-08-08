#ifndef PINOUT_H
#define PINOUT_H
#include "main.h"
#include "usart.h"

#define DEBUG_UART          huart2
#define DEBUG_UART_IRQn     USART2_IRQn

#define LED0_GPIO_Port      GPIOF
#define LED0_Pin            GPIO_PIN_9
#define LED1_GPIO_Port      GPIOF
#define LED1_Pin            GPIO_PIN_10
#define LED_ON_STATE        GPIO_PIN_RESET
#define LED_OFF_STATE       GPIO_PIN_SET

#define KEY0_GPIO_Port      GPIOE
#define KEY0_Pin            GPIO_PIN_4
#define KEY0_PRESSED_STATE  GPIO_PIN_RESET

/* 触摸屏（XPT2046 电阻式，探索者V3 LCD 触摸接口） */
#define TOUCH_CLK_GPIO_Port  GPIOB
#define TOUCH_CLK_Pin        GPIO_PIN_0
#define TOUCH_PEN_GPIO_Port  GPIOB
#define TOUCH_PEN_Pin        GPIO_PIN_1
#define TOUCH_MISO_GPIO_Port GPIOB
#define TOUCH_MISO_Pin       GPIO_PIN_2
#define TOUCH_CS_GPIO_Port   GPIOC
#define TOUCH_CS_Pin         GPIO_PIN_13
#define TOUCH_MOSI_GPIO_Port GPIOF
#define TOUCH_MOSI_Pin       GPIO_PIN_11

/* 板载有源蜂鸣器（高电平发声） */
#define BEEP_GPIO_Port       GPIOF
#define BEEP_Pin             GPIO_PIN_8

/* 外接 MPU6050（I2C1 硬件，400kHz） */
#define MPU6050_SCL_Port     GPIOB
#define MPU6050_SCL_Pin      GPIO_PIN_6
#define MPU6050_SDA_Port     GPIOB
#define MPU6050_SDA_Pin      GPIO_PIN_7

#define HOSTLINK_UART       huart1      /* 涓婁綅鏈洪�氫俊涓撶敤 */
#define HOSTLINK_UART_IRQn  USART1_IRQn
#define HOSTLINK_BAUDRATE   921600

#endif
