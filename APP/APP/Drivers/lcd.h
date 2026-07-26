#ifndef __LCD_H
#define __LCD_H

#include "main.h"

/* LCD重要参数集 */
typedef struct {
    uint16_t width;			/* 屏幕宽度 */
    uint16_t height;		/* 屏幕高度 */
    uint16_t id;			/* LCD ID */
    uint8_t  dir;			/* 横屏还是竖屏：0，竖屏；1，横屏 */
    uint16_t wramcmd;		/* 开始写GRAM指令 */
    uint16_t setxcmd;		/* 设置X坐标指令 */
    uint16_t setycmd;		/* 设置Y坐标指令 */
} _lcd_dev;

extern _lcd_dev lcddev;		/* 管理LCD重要参数 */

/* LCD背光控制 */
#define LCD_BL(x)   do{ x ? HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET) : HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET); }while(0)

/* 扫描方向定义 */
#define L2R_U2D  0				/* 从左到右,从上到下 */
#define DFT_SCAN_DIR  L2R_U2D	/* 默认的扫描方向 */

/* 画笔颜色 */
#define WHITE          0xFFFF
#define BLACK          0x0000
#define RED            0xF800
#define GREEN          0x07E0
#define BLUE           0x001F

void LCD_Init(void);
void LCD_Clear(uint16_t color);
void LCD_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawCircle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);
void LCD_ShowString(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color);
void LCD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color);

#endif