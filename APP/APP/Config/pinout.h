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

/* ¥•√˛∆¡£®XPT2046 µÁ◊Ë Ω£¨ÃΩÀ˜’ﬂV3 LCD ¥•√˛Ω”ø⁄£© */
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

#define HOSTLINK_UART       huart1      /* ‰∏ä‰ΩçÊú∫ÈÄö‰ø°‰∏ìÁî® */
#define HOSTLINK_UART_IRQn  USART1_IRQn
#define HOSTLINK_BAUDRATE   921600

#endif
