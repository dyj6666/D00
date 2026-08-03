#ifndef BSP_GPIO_H
#define BSP_GPIO_H

#include <stdint.h>

/* LED 控制（index: 0 起） */
void BSP_LED_Set(uint8_t index, uint8_t on);
void BSP_LED_Toggle(uint8_t index);

/* 按键状态：1 = 按下 */
uint8_t BSP_KeyPressed(void);

#endif
