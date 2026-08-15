/* ================================================================
 * pinout —— 引脚分配表：外设引脚唯一事实源
 *
 * 架构位置：APP 配置层；改引脚必须先改此处
 * ================================================================ */
#ifndef PINOUT_H
#define PINOUT_H
#include "main.h"
#include "usart.h"

#define DEBUG_UART          huart3      /* 调试/Shell：USART3(PC10=TX PC11=RX) */
#define DEBUG_UART_IRQn     USART3_IRQn

#define LED0_GPIO_Port      GPIOF
#define LED0_Pin            GPIO_PIN_9
#define LED1_GPIO_Port      GPIOF
#define LED1_Pin            GPIO_PIN_10
#define LED_ON_STATE        GPIO_PIN_RESET
#define LED_OFF_STATE       GPIO_PIN_SET

#define KEY0_GPIO_Port      GPIOE
#define KEY0_Pin            GPIO_PIN_4
#define KEY0_PRESSED_STATE  GPIO_PIN_RESET

/* 触摸屏（XPT2046 电阻屏，探索者V3 LCD 触摸接口） */
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

/* 串口资源规划（ETH 接入后）：
 *   USART1 PA9/PA10  HOSTLINK
 *   USART3 PC10/PC11 调试/Shell（USART2 让位给 ETH）
 *   UART5  PC12/PD2  摄像头（预留）
 *   USART6 PC6/PC7   ESP32-S3（预留）
 *   UART4  PA0/PA1   牺牲给 ETH */
#define DBG_TX_Port         GPIOC
#define DBG_TX_Pin          GPIO_PIN_10
#define DBG_RX_Port         GPIOC
#define DBG_RX_Pin          GPIO_PIN_11
#define CAM_UART_TX_Port    GPIOC
#define CAM_UART_TX_Pin     GPIO_PIN_12
#define CAM_UART_RX_Port    GPIOD
#define CAM_UART_RX_Pin     GPIO_PIN_2
#define ESP32_UART_TX_Port  GPIOC
#define ESP32_UART_TX_Pin   GPIO_PIN_6
#define ESP32_UART_RX_Port  GPIOC
#define ESP32_UART_RX_Pin   GPIO_PIN_7

#define HOSTLINK_UART       huart1      /* 上位机通信专用 */
#define HOSTLINK_UART_IRQn  USART1_IRQn
#define HOSTLINK_BAUDRATE   921600

/* CAN1（探索者V3 板载 TJA1050；P5 跳线帽必须拨到 CAN 侧，勿与 USB OTG 共用） */
#define CAN1_RX_GPIO_Port   GPIOA
#define CAN1_RX_Pin         GPIO_PIN_11
#define CAN1_TX_GPIO_Port   GPIOA
#define CAN1_TX_Pin         GPIO_PIN_12

/* On-board W25Q128 SPI Flash: SPI1 remap PB3/PB4/PB5 + PB14 CS.
 * NOTE: PB3/PB4 default to JTAG, driver releases them and switches to SW-DP. */
#define W25Q_CS_GPIO_Port   GPIOB
#define W25Q_CS_Pin         GPIO_PIN_14
#define W25Q_SPI            SPI1

#endif
