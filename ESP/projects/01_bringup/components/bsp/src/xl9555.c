/* ================================================================
 * xl9555.c —— XL9555 16 位 IO 扩展芯片驱动
 *
 * 寄存器（与厂商 BSP 一致）：
 *   0x00 输入 P0    0x01 输入 P1
 *   0x02 输出 P0    0x03 输出 P1     <- 可连写（0x02 起 2 字节）
 *   0x06 方向 P0    0x07 方向 P1     <- 1 = 输入，0 = 输出
 *
 * 方向配置与原理图网表对应：
 *   输入：P0_0(AP_INT) P0_1(QMA_INT) P0_3(KEY1) P0_4(KEY0) + P1_1(CTP_INT) + P1_2~P1_7(扩展排针)
 *   输出：P0_2(BEEP) P0_5(SPK_EN) P0_6(CTP_RST) P0_7(LCD_BL) P1_0(LED_R)
 * ================================================================ */
#include "bsp/xl9555.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BSP/XL9555";

/* ---------------- 寄存器 ---------------- */

#define XL9555_REG_INPUT_P0     0x00U
#define XL9555_REG_INPUT_P1     0x01U
#define XL9555_REG_OUTPUT_P0    0x02U
#define XL9555_REG_OUTPUT_P1    0x03U
#define XL9555_REG_CONFIG_P0    0x06U
#define XL9555_REG_CONFIG_P1    0x07U

/* ---------------- 上电缺省值 ---------------- */

/* 方向：1 = 输入。P0 方向 = 0b0001_1011：P0_0/P0_1/P0_3/P0_4 为输入 */
#define XL9555_DIR_P0_INIT      0x1BU
/* P1 方向 = 0b1111_1110：只有 P1_0（红色 LED）是输出 */
#define XL9555_DIR_P1_INIT      0xFEU

/* 输出初值：BEEP=1(关)、SPK_EN=0(功放关)、CTP_RST=0(触摸保持复位)、
 * LCD_BL=0(背光灭)，其余（含未使用的扩展位）为 0 */
#define XL9555_OUT_P0_INIT      0x04U
/* 红色 LED 低电平点亮，初值给 1 = 灭 */
#define XL9555_OUT_P1_INIT      0x01U

/* ---------------- 内部状态 ---------------- */

static bsp_i2c_dev_t     s_dev = NULL;
static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_int_sem = NULL;
static bsp_xl9555_int_cb_t s_int_cb = NULL;
static void             *s_int_cb_ctx = NULL;
static uint16_t          s_out_shadow = ((uint16_t)XL9555_OUT_P1_INIT << 8) | XL9555_OUT_P0_INIT;

/* ---------------- 中断服务 ---------------- */

/**
 * @brief  XL9555 中断引脚 ISR：只给信号量，绝不做 I2C 访问
 */
static void IRAM_ATTR xl9555_int_isr(void *arg)
{
    BSP_UNUSED(arg);

    BaseType_t woken = pdFALSE;

    if (s_int_sem != NULL) {
        xSemaphoreGiveFromISR(s_int_sem, &woken);
    }
    if (s_int_cb != NULL) {
        s_int_cb(s_int_cb_ctx);                 /* 只允许 ISR 安全操作 */
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------- 底层读写 ---------------- */

/**
 * @brief  把影子寄存器整对写回（一次 3 字节事务：reg + P0 + P1）
 */
static esp_err_t xl9555_flush_outputs(void)
{
    const uint8_t buf[3] = {
        XL9555_REG_OUTPUT_P0,
        (uint8_t)(s_out_shadow & 0xFFU),
        (uint8_t)((s_out_shadow >> 8) & 0xFFU),
    };

    return bsp_i2c_write(s_dev, buf, sizeof(buf));
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_xl9555_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;                          /* 幂等 */
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        s_int_sem = xSemaphoreCreateBinary();
        if (s_lock == NULL || s_int_sem == NULL) {
            ESP_LOGE(TAG, "create sync objects failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = bsp_i2c_add_device(BSP_XL9555_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* ① 先读一次输入寄存器：清除上电时可能已存在的中断标志 */
    uint8_t dummy[2];
    (void)bsp_i2c_reg_read(s_dev, XL9555_REG_INPUT_P0, dummy, sizeof(dummy));

    /* ② 先写输出、再配方向：避免引脚切成输出的瞬间输出意外电平 */
    s_out_shadow = ((uint16_t)XL9555_OUT_P1_INIT << 8) | XL9555_OUT_P0_INIT;
    err = xl9555_flush_outputs();
    if (err != ESP_OK) {
        goto fail;
    }

    err = bsp_i2c_reg_write_u8(s_dev, XL9555_REG_CONFIG_P0, XL9555_DIR_P0_INIT);
    if (err != ESP_OK) {
        goto fail;
    }
    err = bsp_i2c_reg_write_u8(s_dev, XL9555_REG_CONFIG_P1, XL9555_DIR_P1_INIT);
    if (err != ESP_OK) {
        goto fail;
    }

    /* ③ 中断引脚：下降沿触发 + 上拉（芯片 INT 为开漏） */
    const gpio_config_t int_cfg = {
        .pin_bit_mask = BSP_BIT(BSP_XL9555_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      /* 默认关闭，需要时再开 */
    };
    gpio_config(&int_cfg);

    ESP_LOGI(TAG, "ready (out=0x%04X dir P0=0x%02X P1=0x%02X)",
             (unsigned)s_out_shadow, XL9555_DIR_P0_INIT, XL9555_DIR_P1_INIT);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    (void)bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t bsp_xl9555_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }

    (void)bsp_xl9555_int_enable(false);

    /* 全部改成输入，避免芯片仍驱动背光/蜂鸣器等 */
    (void)bsp_i2c_reg_write_u8(s_dev, XL9555_REG_CONFIG_P0, 0xFFU);
    (void)bsp_i2c_reg_write_u8(s_dev, XL9555_REG_CONFIG_P1, 0xFFU);

    esp_err_t err = bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

/* ---------------- 输出 ---------------- */

esp_err_t bsp_xl9555_write_mask(uint16_t mask, uint16_t value)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mask == 0U) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BSP_I2C_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_out_shadow = (uint16_t)((s_out_shadow & ~mask) | (value & mask));
    esp_err_t err = xl9555_flush_outputs();

    (void)xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bsp_xl9555_pin_write(uint16_t pin, bool level)
{
    return bsp_xl9555_write_mask(pin, level ? pin : 0U);
}

bool bsp_xl9555_pin_read(uint16_t pin)
{
    uint16_t inputs = 0;

    if (bsp_xl9555_inputs(&inputs) != ESP_OK) {
        return false;
    }
    return (inputs & pin) != 0U;
}

esp_err_t bsp_xl9555_inputs(uint16_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[2] = {0};
    esp_err_t err = bsp_i2c_reg_read(s_dev, XL9555_REG_INPUT_P0, buf, sizeof(buf));
    if (err == ESP_OK) {
        *out = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
    }
    return err;
}

uint16_t bsp_xl9555_outputs(void)
{
    return s_out_shadow;
}

/* ---------------- 中断 ---------------- */

esp_err_t bsp_xl9555_attach_int(bsp_xl9555_int_cb_t cb, void *ctx)
{
    s_int_cb_ctx = ctx;
    s_int_cb = cb;
    return ESP_OK;
}

esp_err_t bsp_xl9555_detach_int(void)
{
    s_int_cb = NULL;
    s_int_cb_ctx = NULL;
    return ESP_OK;
}

esp_err_t bsp_xl9555_int_enable(bool enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* GPIO ISR 服务是全局单例：已安装时返回 ESP_ERR_INVALID_STATE，视作成功 */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "install isr service failed: %s", esp_err_to_name(err));
        return err;
    }

    if (enable) {
        err = gpio_set_intr_type(BSP_XL9555_INT_GPIO, GPIO_INTR_NEGEDGE);
        if (err == ESP_OK) {
            err = gpio_isr_handler_add(BSP_XL9555_INT_GPIO, xl9555_int_isr, NULL);
            if (err == ESP_ERR_INVALID_STATE) {
                err = ESP_OK;                   /* 已注册过 */
            }
        }
        if (err == ESP_OK) {
            (void)bsp_xl9555_int_clear();       /* 先清掉历史标志，避免立即假触发 */
            err = gpio_intr_enable(BSP_XL9555_INT_GPIO);
        }
    } else {
        (void)gpio_intr_disable(BSP_XL9555_INT_GPIO);
        (void)gpio_isr_handler_remove(BSP_XL9555_INT_GPIO);
        err = ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "int %s failed: %s", enable ? "enable" : "disable", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_xl9555_int_wait(uint32_t timeout_ms)
{
    if (s_int_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_int_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bsp_xl9555_int_clear(void)
{
    uint16_t inputs = 0;
    return bsp_xl9555_inputs(&inputs);
}

void bsp_xl9555_int_flush(void)
{
    if (s_int_sem != NULL) {
        (void)xSemaphoreTake(s_int_sem, 0);
    }
}
