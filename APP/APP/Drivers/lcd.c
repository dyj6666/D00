#include "lcd.h"
#include "stm32f4xx_hal.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* LCD地址结构体 */
typedef struct {
    volatile uint16_t LCD_REG;
    volatile uint16_t LCD_RAM;
} LCD_TypeDef;

/* 基地址，使用FSMC NE4，A6作为RS */
#define LCD_BASE    ((uint32_t)(0x6C000000 | ((1 << 6) * 2 - 2)))
#define LCD         ((LCD_TypeDef *)LCD_BASE)

_lcd_dev lcddev;

/* 写寄存器 */
static void LCD_WR_REG(uint16_t reg) {
    LCD->LCD_REG = reg;
}

/* 写数据 */
static void LCD_WR_DATA(uint16_t data) {
    LCD->LCD_RAM = data;
}

/* 读数据 */
static uint16_t LCD_RD_DATA(void) {
    return LCD->LCD_RAM;
}

/* 设置光标位置 */
static void LCD_SetCursor(uint16_t x, uint16_t y) {
    LCD_WR_REG(lcddev.setxcmd);
    LCD_WR_DATA(x >> 8);
    LCD_WR_DATA(x & 0xFF);
    LCD_WR_REG(lcddev.setycmd);
    LCD_WR_DATA(y >> 8);
    LCD_WR_DATA(y & 0xFF);
}

/* 准备写GRAM */
static void LCD_WriteRAM_Prepare(void) {
    LCD_WR_REG(lcddev.wramcmd);
}

/* 扫描方向设置 */
static void LCD_ScanDir(uint8_t dir) {
    uint16_t regval = 0;
    switch (dir) {
        case L2R_U2D: regval = (0 << 7) | (0 << 6) | (0 << 5); break;
        default: break;
    }
    if (lcddev.id == 0x9341) regval |= 0x08; /* BGR */
    LCD_WR_REG(0x36);
    LCD_WR_DATA(regval);
}

/* ILI9341初始化序列 */
static void LCD_ILI9341_Init(void) {
    LCD_WR_REG(0xCF); LCD_WR_DATA(0x00); LCD_WR_DATA(0xC1); LCD_WR_DATA(0x30);
    LCD_WR_REG(0xED); LCD_WR_DATA(0x64); LCD_WR_DATA(0x03); LCD_WR_DATA(0x12); LCD_WR_DATA(0x81);
    LCD_WR_REG(0xE8); LCD_WR_DATA(0x85); LCD_WR_DATA(0x10); LCD_WR_DATA(0x7A);
    LCD_WR_REG(0xCB); LCD_WR_DATA(0x39); LCD_WR_DATA(0x2C); LCD_WR_DATA(0x00); LCD_WR_DATA(0x34); LCD_WR_DATA(0x02);
    LCD_WR_REG(0xF7); LCD_WR_DATA(0x20);
    LCD_WR_REG(0xEA); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);
    LCD_WR_REG(0xC0); LCD_WR_DATA(0x1B);
    LCD_WR_REG(0xC1); LCD_WR_DATA(0x01);
    LCD_WR_REG(0xC5); LCD_WR_DATA(0x30); LCD_WR_DATA(0x30);
    LCD_WR_REG(0xC7); LCD_WR_DATA(0xB7);
    LCD_WR_REG(0x36); LCD_WR_DATA(0x48);
    LCD_WR_REG(0x3A); LCD_WR_DATA(0x55);
    LCD_WR_REG(0xB1); LCD_WR_DATA(0x00); LCD_WR_DATA(0x1A);
    LCD_WR_REG(0xB6); LCD_WR_DATA(0x0A); LCD_WR_DATA(0xA2);
    LCD_WR_REG(0xF2); LCD_WR_DATA(0x00);
    LCD_WR_REG(0x26); LCD_WR_DATA(0x01);
    LCD_WR_REG(0xE0); LCD_WR_DATA(0x0F); LCD_WR_DATA(0x2A); LCD_WR_DATA(0x28); LCD_WR_DATA(0x08); LCD_WR_DATA(0x0E); LCD_WR_DATA(0x08); LCD_WR_DATA(0x54); LCD_WR_DATA(0xA9); LCD_WR_DATA(0x43); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x0F); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);
    LCD_WR_REG(0xE1); LCD_WR_DATA(0x00); LCD_WR_DATA(0x15); LCD_WR_DATA(0x17); LCD_WR_DATA(0x07); LCD_WR_DATA(0x11); LCD_WR_DATA(0x06); LCD_WR_DATA(0x2B); LCD_WR_DATA(0x56); LCD_WR_DATA(0x3C); LCD_WR_DATA(0x05); LCD_WR_DATA(0x10); LCD_WR_DATA(0x0F); LCD_WR_DATA(0x3F); LCD_WR_DATA(0x3F); LCD_WR_DATA(0x0F);
    LCD_WR_REG(0x2B); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0x01); LCD_WR_DATA(0x3F);
    LCD_WR_REG(0x2A); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0xEF);
    LCD_WR_REG(0x11); HAL_Delay(120);
    LCD_WR_REG(0x29); /* Display ON */
}

/* 读取LCD ID */
static uint16_t LCD_ReadID(void) {
    uint16_t id = 0;
    LCD_WR_REG(0xD3);
    id = LCD_RD_DATA(); /* dummy */
    id = LCD_RD_DATA();
    id = LCD_RD_DATA();
    id <<= 8;
    id |= LCD_RD_DATA();
    return id;
}

/* 动态调整写时序为极限速度 */
static void LCD_FastWriteTiming(void) {
    FSMC_NORSRAM_TimingTypeDef fast = {0};
    fast.AddressSetupTime = 2;
    fast.DataSetupTime = 2;
    fast.AccessMode = FSMC_ACCESS_MODE_A;
    FSMC_NORSRAM_Extended_Timing_Init(FSMC_NORSRAM_EXTENDED_DEVICE, &fast,
                                      FSMC_NORSRAM_BANK4, FSMC_EXTENDED_MODE_ENABLE);
}

/* 初始化LCD，完全自包含，不依赖CubeMX的FSMC配置 */
void LCD_Init(void) {
    /* 1. 使能所有需要的时钟 */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_FSMC_CLK_ENABLE();

    /* 2. 初始化背光引脚 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOB, &gpio);
    LCD_BL(1);

    /* 3. 初始化所有FSMC数据线和控制线（复用推挽，高速，FSMC功能） */
    GPIO_InitTypeDef ctrl = {0};
    ctrl.Mode = GPIO_MODE_AF_PP;
    ctrl.Pull = GPIO_PULLUP;
    ctrl.Speed = GPIO_SPEED_FREQ_HIGH;
    ctrl.Alternate = GPIO_AF12_FSMC;

    /* PD0, PD1, PD4, PD5, PD8, PD9, PD10, PD14, PD15 */
    ctrl.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5|
               GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_14|GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &ctrl);

    /* PE7 - PE15 */
    ctrl.Pin = GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|
               GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &ctrl);

    /* PF12 (A6) */
    ctrl.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOF, &ctrl);

    /* PG12 (NE4) */
    ctrl.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOG, &ctrl);

    /* 4. 配置FSMC NE4用于LCD */
    SRAM_HandleTypeDef hlcd;
    FSMC_NORSRAM_TimingTypeDef read_timing = {0}, write_timing = {0};

    hlcd.Instance = FSMC_NORSRAM_DEVICE;
    hlcd.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
    hlcd.Init.NSBank = FSMC_NORSRAM_BANK4;
    hlcd.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
    hlcd.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
    hlcd.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_16;
    hlcd.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
    hlcd.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
    hlcd.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
    hlcd.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
    hlcd.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
    hlcd.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
    hlcd.Init.ExtendedMode = FSMC_EXTENDED_MODE_ENABLE;
    hlcd.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
    hlcd.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
    hlcd.Init.PageSize = FSMC_PAGE_SIZE_NONE;

    read_timing.AddressSetupTime = 15;
    read_timing.DataSetupTime = 60;
    read_timing.AccessMode = FSMC_ACCESS_MODE_A;

    write_timing.AddressSetupTime = 9;
    write_timing.DataSetupTime = 9;
    write_timing.AccessMode = FSMC_ACCESS_MODE_A;

    if (HAL_SRAM_Init(&hlcd, &read_timing, &write_timing) != HAL_OK) {
        // LOG_Printf("LCD FSMC init failed!\r\n");
        while (1);
    }

    /* 5. 识别并初始化LCD控制器 */
    lcddev.id = LCD_ReadID();
    if (lcddev.id == 0x9341) {
        LCD_ILI9341_Init();
        lcddev.width = 240;
        lcddev.height = 320;
        lcddev.setxcmd = 0x2A;
        lcddev.setycmd = 0x2B;
        lcddev.wramcmd = 0x2C;
        // LOG_Printf("LCD ID: ILI9341\r\n");
        LCD_FastWriteTiming();   /* 提速到极限 */
    } else {
        // LOG_Printf("Unknown LCD ID: 0x%04X\r\n", lcddev.id);
        while (1);
    }

    LCD_ScanDir(DFT_SCAN_DIR);
    lcddev.dir = 0;
    LCD_Clear(WHITE);
}

/* 清屏 */
void LCD_Clear(uint16_t color) {
    LCD_Fill(0, 0, lcddev.width - 1, lcddev.height - 1, color);
}

/* 填充矩形（使用循环展开，极限速度） */
void LCD_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color) {
    uint32_t total_pixels = (uint32_t)(ex - sx + 1) * (ey - sy + 1);
    if (total_pixels == 0) return;

    LCD_SetCursor(sx, sy);
    LCD_WriteRAM_Prepare();

    uint32_t blocks = total_pixels / 8;
    uint32_t remain = total_pixels % 8;

    while (blocks--) {
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
        LCD->LCD_RAM = color;
    }
    while (remain--) {
        LCD->LCD_RAM = color;
    }
}

/* 画点 */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color) {
    LCD_SetCursor(x, y);
    LCD_WriteRAM_Prepare();
    LCD->LCD_RAM = color;
}

/* 画线（Bresenham算法） */
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, row, col;
    delta_x = x2 - x1;
    delta_y = y2 - y1;
    row = x1;
    col = y1;
    if (delta_x > 0) incx = 1;
    else if (delta_x == 0) incx = 0;
    else { incx = -1; delta_x = -delta_x; }
    if (delta_y > 0) incy = 1;
    else if (delta_y == 0) incy = 0;
    else { incy = -1; delta_y = -delta_y; }
    distance = (delta_x > delta_y) ? delta_x : delta_y;
    for (t = 0; t <= distance + 1; t++) {
        LCD_DrawPoint(row, col, color);
        xerr += delta_x;
        yerr += delta_y;
        if (xerr > distance) { xerr -= distance; row += incx; }
        if (yerr > distance) { yerr -= distance; col += incy; }
    }
}

/* 画矩形 */
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
    LCD_DrawLine(x1, y1, x2, y1, color);
    LCD_DrawLine(x1, y1, x1, y2, color);
    LCD_DrawLine(x1, y2, x2, y2, color);
    LCD_DrawLine(x2, y1, x2, y2, color);
}

/* 画圆 */
void LCD_DrawCircle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color) {
    int a, b, di;
    a = 0; b = r; di = 3 - (r << 1);
    while (a <= b) {
        LCD_DrawPoint(x0 + a, y0 - b, color);
        LCD_DrawPoint(x0 + b, y0 - a, color);
        LCD_DrawPoint(x0 + b, y0 + a, color);
        LCD_DrawPoint(x0 + a, y0 + b, color);
        LCD_DrawPoint(x0 - a, y0 + b, color);
        LCD_DrawPoint(x0 - b, y0 + a, color);
        LCD_DrawPoint(x0 - a, y0 - b, color);
        LCD_DrawPoint(x0 - b, y0 - a, color);
        a++;
        if (di < 0) di += 4 * a + 6;
        else { di += 10 + 4 * (a - b); b--; }
    }
}

/* 显示字符（需字库，暂留空） */
void LCD_ShowChar(uint16_t x, uint16_t y, char chr, uint8_t size, uint16_t color) {
    /* 后续集成字库 */
}

/* 显示字符串 */
void LCD_ShowString(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color) {
    while (*p) {
        LCD_ShowChar(x, y, *p, size, color);
        x += size / 2;
        p++;
    }
}

/* 显示数字 */
void LCD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color) {
    char buf[12];
    sprintf(buf, "%*d", len, num);
    LCD_ShowString(x, y, 0, 0, size, buf, color);
}