/* ================================================================
 * bsp_gpio —— GPIO 抽象：初始化与读写
 *
 * 架构位置：APP BSP 层；LED/按键/蜂鸣器底层
 * ================================================================ */
#include "bsp_gpio.h"
#include "pinout.h"

void BSP_LED_Set(uint8_t index, uint8_t on)
{
    GPIO_TypeDef *port = (index == 0) ? LED0_GPIO_Port : LED1_GPIO_Port;
    uint16_t pin = (index == 0) ? LED0_Pin : LED1_Pin;
    GPIO_PinState state = on ? LED_ON_STATE : LED_OFF_STATE;
    HAL_GPIO_WritePin(port, pin, state);
}

void BSP_LED_Toggle(uint8_t index)
{
    GPIO_TypeDef *port = (index == 0) ? LED0_GPIO_Port : LED1_GPIO_Port;
    uint16_t pin = (index == 0) ? LED0_Pin : LED1_Pin;
    HAL_GPIO_TogglePin(port, pin);
}

uint8_t BSP_KeyPressed(void)
{
    return (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == KEY0_PRESSED_STATE);
}
