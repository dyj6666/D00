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
#endif
