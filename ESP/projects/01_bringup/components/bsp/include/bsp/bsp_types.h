/* ================================================================
 * bsp_types.h —— BSP 公共类型与工具宏
 *
 * 约定：
 *   1. 所有对外 API 一律返回 esp_err_t（用 ESP_OK 判断成功）；
 *   2. 每个 .c 文件自带 static const char *TAG = "BSP/xxx"，日志前缀统一；
 *   3. 时间参数一律用毫秒（uint32_t），0 表示"不等待"；
 *   4. 所有 init 幂等：重复调用返回 ESP_OK，不重复申请资源。
 * ================================================================ */
#ifndef __BSP_TYPES_H
#define __BSP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 通用工具宏 ---------------- */

/** 取数组元素个数（仅用于真正的数组，不用于指针） */
#define BSP_COUNT_OF(a)         (sizeof(a) / sizeof((a)[0]))

/** 把参数标记为"有意未使用"，避免 -Wextra 警告 */
#define BSP_UNUSED(x)           ((void)(x))

#define BSP_MIN(a, b)           (((a) < (b)) ? (a) : (b))
#define BSP_MAX(a, b)           (((a) > (b)) ? (a) : (b))
#define BSP_CLAMP(v, lo, hi)    (BSP_MIN(BSP_MAX((v), (lo)), (hi)))

/**
 * 位掩码工具
 * @note 必须用 1ULL：ESP32 是 32 位平台，unsigned long 只有 32 位，
 *       而板上引脚号最大到 48（例如 LCD_RD = IO41），1UL << 41 会溢出为 0。
 *       gpio_config_t.pin_bit_mask 本身是 uint64_t，正好匹配。
 */
#define BSP_BIT(n)              (1ULL << (n))

/* ---------------- 默认时序参数 ---------------- */

#define BSP_I2C_TIMEOUT_MS      100U        /* 单次 I2C 事务超时 */
#define BSP_SPI_TIMEOUT_MS      1000U       /* 单次 SPI 事务超时（TF 卡） */
#define BSP_FLUSH_TIMEOUT_MS    2000U       /* 面板刷新等待上限 */

/* ---------------- 模块位掩码（用于 bsp_init / bsp_deinit）---------------- */

/**
 * @brief 可独立启停的板级模块
 * @note  bsp_init() 会自动补齐依赖（例如只要 LCD，会连带 I2C + XL9555）
 */
typedef enum
{
    BSP_MOD_I2C_BUS = 0,        /*!< I2C0 共享总线（多数器件的公共依赖） */
    BSP_MOD_SPI_BUS = 1,        /*!< SPI2 共享总线（TF 卡） */
    BSP_MOD_XL9555  = 2,        /*!< IO 扩展芯片（背光/蜂鸣器/按键/红LED/触摸复位） */
    BSP_MOD_EEPROM  = 3,        /*!< 板载 AT24C02 */
    BSP_MOD_LIGHT   = 4,        /*!< AP3216C 光环境传感器 */
    BSP_MOD_IMU     = 5,        /*!< QMI8658A 六轴 */
    BSP_MOD_TEMP    = 6,        /*!< 片内温度传感器 */
    BSP_MOD_RTC     = 7,        /*!< 片内 RTC / 系统时间 */
    BSP_MOD_ADC     = 8,        /*!< 扩展 IO 上的 ADC */
    BSP_MOD_UART    = 9,        /*!< 外部 UART 口（备用串口，默认 UART1） */
    BSP_MOD_LED     = 10,       /*!< LED（蓝灯 PWM / 红灯 IO 扩展） */
    BSP_MOD_BEEP    = 11,       /*!< 有源蜂鸣器 */
    BSP_MOD_LCD     = 12,       /*!< 2.4 寸 TFT（8080 并口）+ 背光 */
    BSP_MOD_TOUCH   = 13,       /*!< 电容触摸 CHSC5432 */
    BSP_MOD_KEY     = 14,       /*!< 按键（BOOT + KEY0/KEY1） */
    BSP_MOD_AUDIO   = 15,       /*!< I2S 音频 + 编解码/功放 */
    BSP_MOD_TFCARD  = 16,       /*!< TF 卡（FAT 挂载） */
} bsp_module_t;

/** 模块位掩码类型 */
typedef uint32_t bsp_module_mask_t;

/** 单个模块 -> 掩码 */
#define BSP_MOD_MASK(m)         ((bsp_module_mask_t)BSP_BIT((unsigned)(m)))

/** 全部模块（bsp_init 默认值） */
#define BSP_MOD_ALL             ((bsp_module_mask_t)0x0001FFFFu)

/* ---------------- 常用小类型 ---------------- */

/** 三轴浮点向量（加速度/角速度/欧拉角通用） */
typedef struct
{
    float x;
    float y;
    float z;
} bsp_vec3_t;

/** 日历时间（本地时区） */
typedef struct
{
    uint16_t year;      /*!< 完整年份，如 2026 */
    uint8_t  month;     /*!< 1~12 */
    uint8_t  day;       /*!< 1~31 */
    uint8_t  hour;      /*!< 0~23 */
    uint8_t  minute;    /*!< 0~59 */
    uint8_t  second;    /*!< 0~59 */
    uint8_t  weekday;   /*!< 0=周日 */
} bsp_datetime_t;

#ifdef __cplusplus
}
#endif

#endif /* __BSP_TYPES_H */
