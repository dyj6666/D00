/* ================================================================
 * bsp.c —— 板级入口：模块表 + 依赖解析 + 可回滚初始化
 *
 * 设计要点：
 *   1. **表驱动**：每个模块一行 {位、名字、init、deinit、是否可选}，
 *      加新外设只需加一行，不用改流程；
 *   2. **依赖自动补齐**：只要求 LCD 时，I2C 与 XL9555 会被自动带上，
 *      避免"忘了先初始化依赖"这类低级错误；
 *   3. **失败可回滚**：本次调用里已经起来的模块按相反顺序关掉，
 *      不会留下半初始化状态；
 *   4. **可选模块**（TF 卡这类可插拔件）失败只警告、不中断整体初始化。
 *
 * 表内顺序即初始化顺序，必须保证依赖在消费者之前（I2C/SPI 在最前）。
 * ================================================================ */
#include "bsp/bsp.h"

#include "bsp/adc.h"
#include "bsp/audio.h"
#include "bsp/beep.h"
#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"
#include "bsp/bus/spi_bus.h"
#include "bsp/eeprom.h"
#include "bsp/imu.h"
#include "bsp/key.h"
#include "bsp/lcd.h"
#include "bsp/led.h"
#include "bsp/lightsensor.h"
#include "bsp/rtc.h"
#include "bsp/temperature.h"
#include "bsp/tfcard.h"
#include "bsp/touch.h"
#include "bsp/uart.h"
#include "bsp/xl9555.h"

#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "BSP";

/* ---------------- 前置声明 ---------------- */

/** rtc 模块的统一入口（模块表要求回调形如 esp_err_t (*)(void)） */
esp_err_t bsp_rtc_init_default(void);

/** I2C 地址 -> 器件名（仅用于打印） */
const char *bsp_i2c_name_of(uint8_t addr);

/* ---------------- 模块描述表 ---------------- */

typedef struct
{
    bsp_module_t flag;              /*!< 模块位 */
    const char  *name;              /*!< 日志名 */
    esp_err_t  (*init)(void);       /*!< 初始化回调 */
    esp_err_t  (*deinit)(void);     /*!< 反初始化回调（可为 NULL） */
    bool         optional;          /*!< true = 失败只警告不中断 */
} bsp_module_desc_t;

/* 顺序 = 初始化顺序：总线 -> IO 扩展 -> 其它 */
static const bsp_module_desc_t s_modules[] = {
    { BSP_MOD_I2C_BUS, "i2c",         bsp_i2c_init,          bsp_i2c_deinit,          false },
    { BSP_MOD_SPI_BUS, "spi",         bsp_spi_bus_init,      bsp_spi_bus_deinit,      false },
    { BSP_MOD_XL9555,  "xl9555",      bsp_xl9555_init,       bsp_xl9555_deinit,       false },
    { BSP_MOD_EEPROM,  "eeprom",      bsp_eeprom_init,       bsp_eeprom_deinit,       false },
    { BSP_MOD_LIGHT,   "ap3216c",     bsp_light_init,        bsp_light_deinit,        false },
    { BSP_MOD_IMU,     "qmi8658a",    bsp_imu_init,          bsp_imu_deinit,          false },
    { BSP_MOD_TEMP,    "temp",        bsp_temperature_init,  bsp_temperature_deinit,  false },
    { BSP_MOD_RTC,     "rtc",         bsp_rtc_init_default,  NULL,                    false },
    { BSP_MOD_ADC,     "adc",         bsp_adc_init,          bsp_adc_deinit,          false },
    { BSP_MOD_UART,    "uart",        bsp_uart_init,         bsp_uart_deinit,         false },
    { BSP_MOD_LED,     "led",         bsp_led_init,          bsp_led_deinit,          false },
    { BSP_MOD_BEEP,    "beep",        bsp_beep_init,         bsp_beep_deinit,         false },
    { BSP_MOD_LCD,     "lcd",         bsp_lcd_init,          bsp_lcd_deinit,          false },
    { BSP_MOD_TOUCH,   "touch",       bsp_touch_init,        bsp_touch_deinit,        false },
    { BSP_MOD_KEY,     "key",         bsp_key_init,          bsp_key_deinit,          false },
    { BSP_MOD_AUDIO,   "audio",       bsp_audio_init,        bsp_audio_deinit,        false },
    { BSP_MOD_TFCARD,  "tfcard",      bsp_tfcard_init,       bsp_tfcard_deinit,       true  },
};

/* ---------------- 依赖表 ---------------- */

typedef struct
{
    bsp_module_t     module;
    bsp_module_mask_t depends;      /*!< 该模块需要先跑起来的其它模块 */
} bsp_dep_t;

static const bsp_dep_t s_deps[] = {
    { BSP_MOD_XL9555, BSP_MOD_MASK(BSP_MOD_I2C_BUS) },
    { BSP_MOD_EEPROM, BSP_MOD_MASK(BSP_MOD_I2C_BUS) },
    { BSP_MOD_LIGHT,  BSP_MOD_MASK(BSP_MOD_I2C_BUS) },
    { BSP_MOD_IMU,    BSP_MOD_MASK(BSP_MOD_I2C_BUS) },
    { BSP_MOD_LED,    BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_BEEP,   BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_LCD,    BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_TOUCH,  BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_KEY,    BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_AUDIO,  BSP_MOD_MASK(BSP_MOD_XL9555) },
    { BSP_MOD_TFCARD, BSP_MOD_MASK(BSP_MOD_SPI_BUS) },
};

/* ---------------- 运行状态 ---------------- */

static bsp_module_mask_t s_running = 0;

esp_err_t bsp_rtc_init_default(void)
{
    return bsp_rtc_init(NULL);
}

/* ---------------- 依赖解析 ---------------- */

/**
 * @brief  把请求的模块位掩码补齐依赖（迭代到稳定为止）
 */
static bsp_module_mask_t bsp_expand_deps(bsp_module_mask_t want)
{
    bool changed = true;

    while (changed) {
        changed = false;
        for (size_t i = 0; i < BSP_COUNT_OF(s_deps); i++) {
            const bsp_module_mask_t bit = BSP_MOD_MASK(s_deps[i].module);

            if ((want & bit) != 0U) {
                const bsp_module_mask_t add = s_deps[i].depends & ~want;

                if (add != 0U) {
                    want |= add;
                    changed = true;
                }
            }
        }
    }
    return want;
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_init(bsp_module_mask_t modules)
{
    const bsp_module_mask_t want = bsp_expand_deps(modules & BSP_MOD_ALL);
    bsp_module_mask_t started_now = 0;
    esp_err_t result = ESP_OK;

    for (size_t i = 0; i < BSP_COUNT_OF(s_modules); i++) {
        const bsp_module_desc_t *m = &s_modules[i];
        const bsp_module_mask_t bit = BSP_MOD_MASK(m->flag);

        if ((want & bit) == 0U) {
            continue;
        }
        if ((s_running & bit) != 0U) {
            continue;                           /* 已在运行 */
        }

        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = m->init();

        if (err == ESP_OK) {
            s_running |= bit;
            started_now |= bit;
            ESP_LOGD(TAG, "%s init %lld us", m->name, (esp_timer_get_time() - t0));
            continue;
        }

        if (m->optional) {
            ESP_LOGW(TAG, "%s 初始化失败（可选模块，跳过）：%s", m->name, esp_err_to_name(err));
            continue;
        }

        ESP_LOGE(TAG, "%s 初始化失败：%s", m->name, esp_err_to_name(err));
        result = err;
        break;
    }

    if (result != ESP_OK) {
        /* 回滚本次启动的模块（倒序），已成功运行的保持一致 */
        for (size_t i = BSP_COUNT_OF(s_modules); i-- > 0U;) {
            const bsp_module_desc_t *m = &s_modules[i];
            const bsp_module_mask_t bit = BSP_MOD_MASK(m->flag);

            if ((started_now & bit) == 0U) {
                continue;
            }
            if (m->deinit != NULL) {
                (void)m->deinit();
            }
            s_running &= ~bit;
        }
        return result;
    }

    ESP_LOGI(TAG, "init done, running=0x%08lX", (unsigned long)s_running);
    return ESP_OK;
}

esp_err_t bsp_deinit(void)
{
    for (size_t i = BSP_COUNT_OF(s_modules); i-- > 0U;) {
        const bsp_module_desc_t *m = &s_modules[i];
        const bsp_module_mask_t bit = BSP_MOD_MASK(m->flag);

        if ((s_running & bit) == 0U) {
            continue;
        }
        if (m->deinit != NULL) {
            esp_err_t err = m->deinit();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "%s deinit: %s", m->name, esp_err_to_name(err));
            }
        }
        s_running &= ~bit;
    }

    return ESP_OK;
}

bool bsp_module_is_running(bsp_module_t module)
{
    return (s_running & BSP_MOD_MASK(module)) != 0U;
}

bsp_module_mask_t bsp_modules_running(void)
{
    return s_running;
}

/* ---------------- 信息与自检 ---------------- */

const char *bsp_board_name(void)
{
    return "ATK-DNESP32S3B (ESP32S3 BOX / AI BOX1)";
}

const char *bsp_version_string(void)
{
    return "1.0.0";
}

int bsp_i2c_scan(uint8_t *found, size_t max)
{
    if (found == NULL || max == 0U) {
        return -1;
    }

    int count = 0;

    for (uint16_t addr = 0x08U; addr <= 0x77U; addr++) {
        if (bsp_i2c_probe(addr, 2U)) {
            if ((size_t)count < max) {
                found[count] = (uint8_t)addr;
            }
            count++;
        }
    }
    return count;
}

void bsp_print_info(void)
{
    esp_chip_info_t chip;

    esp_chip_info(&chip);

    ESP_LOGI(TAG, "================ BSP %s ================", bsp_version_string());
    ESP_LOGI(TAG, "board : %s", bsp_board_name());
    ESP_LOGI(TAG, "chip  : ESP32-S3 rev v%d.%d, %d core(s)（Flash/PSRAM 由应用自检打印）",
             chip.revision / 100, chip.revision % 100, chip.cores);

    ESP_LOGI(TAG, "---- modules ----");
    for (size_t i = 0; i < BSP_COUNT_OF(s_modules); i++) {
        const bsp_module_desc_t *m = &s_modules[i];

        ESP_LOGI(TAG, "  %-8s : %s", m->name,
                 ((s_running & BSP_MOD_MASK(m->flag)) != 0U) ? "running" : "-");
    }

    if (bsp_module_is_running(BSP_MOD_I2C_BUS)) {
        uint8_t found[16];
        const int n = bsp_i2c_scan(found, BSP_COUNT_OF(found));

        if (n < 0) {
            ESP_LOGW(TAG, "i2c scan: bus unavailable");
        } else {
            ESP_LOGI(TAG, "---- i2c0 scan (%d device(s)) ----", n);
            for (int i = 0; i < n && i < (int)BSP_COUNT_OF(found); i++) {
                ESP_LOGI(TAG, "  0x%02X %s", (unsigned)found[i], bsp_i2c_name_of(found[i]));
            }
        }
    }

    ESP_LOGI(TAG, "pin : LCD(CS=%d DC=%d WR=%d RD=%d)", (int)BSP_LCD_PIN_CS,
             (int)BSP_LCD_PIN_DC, (int)BSP_LCD_PIN_WR, (int)BSP_LCD_PIN_RD);
    ESP_LOGI(TAG, "pin : I2C0(SDA=%d SCL=%d) SPI2(SCK=%d MISO=%d MOSI=%d CS=%d)",
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO, (int)BSP_SPI_SCK_GPIO,
             (int)BSP_SPI_MISO_GPIO, (int)BSP_SPI_MOSI_GPIO, (int)BSP_SPI_TF_CS_GPIO);
    ESP_LOGI(TAG, "pin : I2S(BCLK=%d WS=%d DOUT=%d DIN=%d) LED=%d BOOT=%d XL9555_INT=%d",
             (int)BSP_I2S_BCLK_GPIO, (int)BSP_I2S_WS_GPIO, (int)BSP_I2S_DOUT_GPIO,
             (int)BSP_I2S_DIN_GPIO, (int)BSP_LED_BLUE_GPIO, (int)BSP_KEY_BOOT_GPIO,
             (int)BSP_XL9555_INT_GPIO);
    ESP_LOGI(TAG, "==============================================");
}

const char *bsp_i2c_name_of(uint8_t addr)
{
    switch (addr) {
    case BSP_XL9555_I2C_ADDR:   return "XL9555  (IO expander)";
    case BSP_EEPROM_I2C_ADDR:   return "AT24C02 (EEPROM)";
    case BSP_AP3216C_I2C_ADDR:  return "AP3216C (light/proximity)";
    case BSP_QMI8658A_I2C_ADDR: return "QMI8658A (6-axis IMU)";
    case BSP_CHSC5432_I2C_ADDR: return "CHSC5432 (touch)";
    case BSP_ES8311_I2C_ADDR:   return "ES8311  (audio codec)";
    default:                    return "(unknown)";
    }
}

esp_err_t bsp_selftest(bool report)
{
    esp_err_t result = ESP_OK;

    if (!bsp_module_is_running(BSP_MOD_I2C_BUS)) {
        /* 自检自己把 I2C 拉起来：只读操作，安全 */
        esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 板上固定存在的 I2C 器件：必须应答 */
    static const struct
    {
        uint8_t     addr;
        const char *name;
    } required[] = {
        { BSP_XL9555_I2C_ADDR,   "XL9555" },
        { BSP_EEPROM_I2C_ADDR,   "AT24C02" },
        { BSP_AP3216C_I2C_ADDR,  "AP3216C" },
        { BSP_QMI8658A_I2C_ADDR, "QMI8658A" },
        { BSP_CHSC5432_I2C_ADDR, "CHSC5432" },
    };

    for (size_t i = 0; i < BSP_COUNT_OF(required); i++) {
        const bool ok = bsp_i2c_probe(required[i].addr, BSP_I2C_TIMEOUT_MS);

        if (report) {
            ESP_LOGI(TAG, "selftest %-9s @0x%02X : %s", required[i].name,
                     (unsigned)required[i].addr, ok ? "OK" : "MISSING");
        }
        if (!ok) {
            result = ESP_ERR_NOT_FOUND;
        }
    }

    /* 音频编解码是"两种板型二选一"，只报告不判失败 */
    if (report) {
        const bool es = bsp_i2c_probe(BSP_ES8311_I2C_ADDR, BSP_I2C_TIMEOUT_MS);

        ESP_LOGI(TAG, "selftest audio     @0x%02X : %s", (unsigned)BSP_ES8311_I2C_ADDR,
                 es ? "ES8311 present" : "NS4168 (pure I2S)");
    }

    if (report) {
        ESP_LOGI(TAG, "selftest result: %s", (result == ESP_OK) ? "PASS" : "FAIL");
    }
    return result;
}
