/* ================================================================
 * lightsensor.c —— AP3216C 驱动
 *
 * 寄存器：
 *   0x00 系统控制（写 0x04 = 软复位；写 0x03 = 开 ALS + PS + IR）
 *   0x0A~0x0F 数据：IR_L IR_H ALS_L ALS_H PS_L PS_H
 *   0x01 ALS 配置，0x02 PS 配置（增益/量程，见数据手册）
 *
 * 数据拼接（与厂商驱动一致）：
 *   IR  = (buf[1] << 2) | (buf[0] & 0x03)      buf[0] bit7 = 1 表示数据无效
 *   ALS = (buf[3] << 8) | buf[2]
 *   PS  = ((buf[5] & 0x3F) << 4) | (buf[4] & 0x0F)   buf[4] bit6 = 1 表示无效
 * ================================================================ */
#include "bsp/lightsensor.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"
#include "bsp/xl9555.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/ALS";

#define AP3216C_REG_SYS         0x00U
#define AP3216C_REG_ALS_CFG     0x01U
#define AP3216C_REG_PS_CFG      0x02U
#define AP3216C_REG_DATA_START  0x0AU

#define AP3216C_SYS_SW_RESET    0x04U
#define AP3216C_SYS_ALS_PS_IR   0x03U

#define AP3216C_RESET_DELAY_MS  50U

static bsp_i2c_dev_t s_dev = NULL;

esp_err_t bsp_light_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_add_device(BSP_AP3216C_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    if (!bsp_i2c_probe(BSP_AP3216C_I2C_ADDR, BSP_I2C_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "device not found at 0x%02X", BSP_AP3216C_I2C_ADDR);
        (void)bsp_i2c_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* ① 软复位（数据手册要求复位后至少等 10ms，这里取 50ms 保险） */
    err = bsp_i2c_reg_write_u8(s_dev, AP3216C_REG_SYS, AP3216C_SYS_SW_RESET);
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(AP3216C_RESET_DELAY_MS));

    /* ② 开启 ALS + PS + IR */
    err = bsp_i2c_reg_write_u8(s_dev, AP3216C_REG_SYS, AP3216C_SYS_ALS_PS_IR);
    if (err != ESP_OK) {
        goto fail;
    }

    /* ③ 回读校验（厂商例程同款做法，能一次确认器件真的在） */
    uint8_t check = 0;
    err = bsp_i2c_reg_read_u8(s_dev, AP3216C_REG_SYS, &check);
    if (err != ESP_OK) {
        goto fail;
    }
    if (check != AP3216C_SYS_ALS_PS_IR) {
        ESP_LOGE(TAG, "config verify failed: got 0x%02X", (unsigned)check);
        err = ESP_FAIL;
        goto fail;
    }

    ESP_LOGI(TAG, "ready (ALS+PS+IR on, 全开模式采样间隔需 >112.5ms)");
    return ESP_OK;

fail:
    (void)bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t bsp_light_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }

    (void)bsp_i2c_reg_write_u8(s_dev, AP3216C_REG_SYS, 0x00U);  /* 掉电 */

    esp_err_t err = bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t bsp_light_read(bsp_light_data_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[6] = {0};

    esp_err_t err = bsp_i2c_reg_read(s_dev, AP3216C_REG_DATA_START, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* IR：bit7 置位表示本次数据无效 */
    out->ir = (buf[0] & 0x80U) ? 0U : (uint16_t)(((uint16_t)buf[1] << 2) | (buf[0] & 0x03U));

    /* ALS 直接是 16bit */
    out->als = (uint16_t)(((uint16_t)buf[3] << 8) | buf[2]);

    /* PS：bit6 置位表示无效；有效时只有 10bit（低 4 位 + 高 6 位） */
    out->ps = (buf[4] & 0x40U) ? 0U
                              : (uint16_t)(((uint16_t)(buf[5] & 0x3FU) << 4) | (buf[4] & 0x0FU));
    return ESP_OK;
}

esp_err_t bsp_light_read_als(uint16_t *als)
{
    if (s_dev == NULL || als == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[2] = {0};
    esp_err_t err = bsp_i2c_reg_read(s_dev, 0x0CU, buf, sizeof(buf));   /* ALS_L 起始 */
    if (err == ESP_OK) {
        *als = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
    }
    return err;
}

bool bsp_light_irq_asserted(void)
{
    /* AP_INT 为低有效，接在 XL9555 P0_0 */
    return !bsp_xl9555_pin_read(BSP_XL9555_PIN_AP_INT);
}

esp_err_t bsp_light_write_reg(uint8_t reg, uint8_t val)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_i2c_reg_write_u8(s_dev, reg, val);
}

esp_err_t bsp_light_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_i2c_reg_read_u8(s_dev, reg, val);
}
