/* ================================================================
 * bsp_pinmap.h —— 全板引脚/地址"单一事实源"
 *
 * 数据来源（三份交叉核对）：
 *   1. 《ESP32S3 BOX 硬件参考手册》1.2.2 IO 资源分配表
 *   2. 原理图 ATK_DNESP32S3B_V1.1.pdf 网表
 *   3. 厂商例程 BSP 头文件里的实际定义（v5.2.1 / v5.4）
 *
 * 已知文档错误（本文件以原理图与实际为准）：
 *   - 手册把 IO41/IO42 写成 IIC_SDA/IIC_SCL，实际是 LCD_RD/LCD_WR；
 *     I2C0 是 IO48(SDA)/IO45(SCL)，且这两根同时接电容触摸。
 *   - 手册 2.1.16 节称 LCD 接口"仅支持 RGB 屏"，实际是 8bit 8080 MCU 接口。
 *
 * 硬件相关的修改只改这里，驱动层不允许出现其它裸引脚号。
 * ================================================================ */
#ifndef __BSP_PINMAP_H
#define __BSP_PINMAP_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 共享总线 ================= */

/* ---- I2C0：XL9555 / AT24C02 / AP3216C / QMI8658A / CHSC5432 / ES8311 ---- */
#define BSP_I2C_PORT            (0)                 /* I2C_NUM_0 */
#define BSP_I2C_SDA_GPIO        GPIO_NUM_48
#define BSP_I2C_SCL_GPIO        GPIO_NUM_45
#define BSP_I2C_SPEED_HZ        (400000U)

/* ---- SPI2：TF 卡（本板 LCD 走 8080 并口，故 SPI2 由 TF 卡独占）---- */
#define BSP_SPI_HOST            (2)                 /* SPI2_HOST */
#define BSP_SPI_SCK_GPIO        GPIO_NUM_7
#define BSP_SPI_MISO_GPIO       GPIO_NUM_15
#define BSP_SPI_MOSI_GPIO       GPIO_NUM_16
#define BSP_SPI_TF_CS_GPIO      GPIO_NUM_17
#define BSP_SPI_TF_FREQ_HZ      (20000000U)         /* TF 卡时钟 20MHz */

/* ---- I2S0：音频编解码/功放 + 数字麦克风（无 MCLK，编解码取 BCLK 当 MCLK）---- */
#define BSP_I2S_PORT            (0)                 /* I2S_NUM_0 */
#define BSP_I2S_BCLK_GPIO       GPIO_NUM_21
#define BSP_I2S_WS_GPIO         GPIO_NUM_13
#define BSP_I2S_DOUT_GPIO       GPIO_NUM_14         /* ESP32 -> 编解码/功放 */
#define BSP_I2S_DIN_GPIO        GPIO_NUM_47         /* MIC -> ESP32 */
#define BSP_I2S_MCLK_GPIO       GPIO_NUM_NC         /* 板上未连接 */

/* ================= LCD（2.4 寸 320x240 ST7789VW，8080 8bit 并口）================= */

#define BSP_LCD_WIDTH           320
#define BSP_LCD_HEIGHT          240

#define BSP_LCD_PIN_CS          GPIO_NUM_1
#define BSP_LCD_PIN_DC          GPIO_NUM_2          /* 手册里的 LCD_RS */
#define BSP_LCD_PIN_WR          GPIO_NUM_42
#define BSP_LCD_PIN_RD          GPIO_NUM_41
#define BSP_LCD_PIN_RST         GPIO_NUM_NC         /* 复位与模组 EN 共用 */

#define BSP_LCD_PIN_D0          GPIO_NUM_40
#define BSP_LCD_PIN_D1          GPIO_NUM_39
#define BSP_LCD_PIN_D2          GPIO_NUM_38
#define BSP_LCD_PIN_D3          GPIO_NUM_12
#define BSP_LCD_PIN_D4          GPIO_NUM_11
#define BSP_LCD_PIN_D5          GPIO_NUM_10
#define BSP_LCD_PIN_D6          GPIO_NUM_9
#define BSP_LCD_PIN_D7          GPIO_NUM_46

#define BSP_LCD_PCLK_HZ         (15000000U)         /* 8080 像素时钟 15MHz（ST7789 写周期下限 66ns≈15.1MHz） */

/* ================= 独立 GPIO ================= */

#define BSP_LED_BLUE_GPIO       GPIO_NUM_4          /* 0805 蓝色灯，低电平点亮 */
#define BSP_KEY_BOOT_GPIO       GPIO_NUM_0          /* BOOT 键，低电平有效 */

/* 扩展排针上的完全独立 IO（J3/J4），可按需复用为 UART / ADC / 中断等 */
#define BSP_EXP_IO_5            GPIO_NUM_5
#define BSP_EXP_IO_6            GPIO_NUM_6
#define BSP_EXP_IO_8            GPIO_NUM_8
#define BSP_EXP_IO_18           GPIO_NUM_18

/* 外部串口（与 STM32 联调预留）：默认借用扩展 IO5( RX)/IO6(TX) */
#define BSP_AUX_UART_PORT       (1)                 /* UART_NUM_1 */
#define BSP_AUX_UART_TX_GPIO    BSP_EXP_IO_6
#define BSP_AUX_UART_RX_GPIO    BSP_EXP_IO_5
#define BSP_AUX_UART_BAUD       (115200)

/* 扩展 IO 上的 ADC（IO18 属 ADC2 且与 WiFi 冲突，故只提供 ADC1 三路） */
#define BSP_ADC_UNIT_ID         (1)                 /* ADC_UNIT_1 */
#define BSP_ADC_CH_IO5          (4)                 /* ADC1_CH4 */
#define BSP_ADC_CH_IO6          (5)                 /* ADC1_CH5 */
#define BSP_ADC_CH_IO8          (7)                 /* ADC1_CH7 */

/* ================= I2C 从器件地址（7bit）================= */

#define BSP_XL9555_I2C_ADDR     0x20        /* A2/A1/A0 = 0 */
#define BSP_XL9555_INT_GPIO     GPIO_NUM_3  /* 中断输出，低有效，读输入寄存器清除 */
#define BSP_EEPROM_I2C_ADDR     0x50        /* 24C02，A0~A2 接地 */
#define BSP_AP3216C_I2C_ADDR    0x1E
#define BSP_QMI8658A_I2C_ADDR   0x6A        /* SA0=1 */
#define BSP_CHSC5432_I2C_ADDR   0x2E        /* 电容触摸 */
#define BSP_ES8311_I2C_ADDR     0x18        /* CE=0 */

/* ================= XL9555 引脚（P0 = 低 8 位，P1 = 高 8 位）=================
 * 与原理图 DEVICE.SchDoc 网表一一对应，不要改动顺序
 */
#define BSP_XL9555_PIN_AP_INT   0x0001U     /* P0_0  AP3216C 中断（输入） */
#define BSP_XL9555_PIN_QMA_INT  0x0002U     /* P0_1  QMI8658A 中断（输入） */
#define BSP_XL9555_PIN_BEEP     0x0004U     /* P0_2  蜂鸣器（1 = 关） */
#define BSP_XL9555_PIN_KEY1     0x0008U     /* P0_3  KEY1（输入，低有效） */
#define BSP_XL9555_PIN_KEY0     0x0010U     /* P0_4  KEY0（输入，低有效） */
#define BSP_XL9555_PIN_SPK_EN   0x0020U     /* P0_5  音频功放使能 */
#define BSP_XL9555_PIN_CTP_RST  0x0040U     /* P0_6  触摸复位 */
#define BSP_XL9555_PIN_LCD_BL   0x0080U     /* P0_7  LCD 背光（1 = 亮） */
#define BSP_XL9555_PIN_LED_R    0x0100U     /* P1_0  红色 LED（0 = 亮） */
#define BSP_XL9555_PIN_CTP_INT  0x0200U     /* P1_1  触摸中断（输入） */
#define BSP_XL9555_PIN_EX1      0x0400U     /* P1_2  扩展排针 J2 */
#define BSP_XL9555_PIN_EX2      0x0800U     /* P1_3  扩展排针 J2 */
#define BSP_XL9555_PIN_EX3      0x1000U     /* P1_4  扩展排针 J2 */
#define BSP_XL9555_PIN_EX4      0x2000U     /* P1_5  扩展排针 J2 */
#define BSP_XL9555_PIN_EX5      0x4000U     /* P1_6  扩展排针 J2 */
#define BSP_XL9555_PIN_EX6      0x8000U     /* P1_7  扩展排针 J2 */

#ifdef __cplusplus
}
#endif

#endif /* __BSP_PINMAP_H */
