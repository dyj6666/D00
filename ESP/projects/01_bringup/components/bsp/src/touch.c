/* ================================================================
 * touch.c —— CHSC5432 电容触摸驱动
 *
 * 数据格式（官方给的 28 字节块）：
 *   buf[0]     保留
 *   buf[1]     低 4 位 = 本次上报的点数
 *   buf[2+5i]  点 i 的 X 低字节 / 竖屏时用作 X 低 8 位
 *   buf[3+5i]  点 i 的 X 高字节 / 竖屏时用作 Y 低 8 位
 *   buf[4+5i]  点 i 的保留字节
 *   buf[5+5i]  点 i 的高位字节：高 4 位含 X 的高 8 位，低 4 位含 Y 的高 8 位
 *   buf[6+5i]  点 i 的保留字节
 *
 * 换算公式直接沿用厂商驱动（CHSC5xxx 官方算法），只补上坐标区间校验。
 * ================================================================ */
#include "bsp/touch.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"
#include "bsp/xl9555.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/TOUCH";

#define CHSC_REG_CTRL           0x2000002CU    /* 触摸事件（28 字节） */
#define CHSC_REG_PID            0x20000080U    /* 产品 ID（4 字节） */

#define CHSC_READ_LEN           28U
#define CHSC_POINT_STRIDE       5U
#define CHSC_POINT_BASE_OFFSET  2U

#define CHSC_RESET_DELAY_MS     100U

static bsp_i2c_dev_t s_dev = NULL;
static bool          s_ready = false;

/* ---------------- 底层寄存器访问 ---------------- */

/**
 * @brief  把 32bit 寄存器地址拆成大端 4 字节
 */
static void chsc_pack_addr(uint32_t reg, uint8_t out[4])
{
    out[0] = (uint8_t)((reg >> 24) & 0xFFU);
    out[1] = (uint8_t)((reg >> 16) & 0xFFU);
    out[2] = (uint8_t)((reg >> 8) & 0xFFU);
    out[3] = (uint8_t)(reg & 0xFFU);
}

/**
 * @brief  读连续寄存器（地址 4 字节 + 重复起始 + 读数据，一次事务）
 */
static esp_err_t chsc_rd_reg(uint32_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[4];

    chsc_pack_addr(reg, addr);
    return bsp_i2c_write_read(s_dev, addr, sizeof(addr), buf, len);
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_touch_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_add_device(BSP_CHSC5432_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    err = bsp_touch_reset();
    if (err != ESP_OK) {
        goto fail;
    }

    uint32_t pid = 0;
    err = bsp_touch_read_pid(&pid);
    if (err != ESP_OK) {
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (CHSC5432 PID=0x%06X, %s屏)",
             (unsigned)(pid & 0xFFFFFFU), BSP_TOUCH_LANDSCAPE ? "横" : "竖");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    (void)bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t bsp_touch_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }

    (void)bsp_xl9555_pin_write(BSP_XL9555_PIN_CTP_RST, false);  /* 保持复位 */

    esp_err_t err = bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    s_ready = false;
    return err;
}

esp_err_t bsp_touch_reset(void)
{
    esp_err_t err = bsp_xl9555_pin_write(BSP_XL9555_PIN_CTP_RST, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(CHSC_RESET_DELAY_MS));

    return bsp_xl9555_pin_write(BSP_XL9555_PIN_CTP_RST, true);
}

/* ---------------- 读取 ---------------- */

esp_err_t bsp_touch_read_raw(bsp_touch_state_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[CHSC_READ_LEN] = {0};

    esp_err_t err = chsc_rd_reg(CHSC_REG_CTRL, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t count = (uint8_t)(buf[1] & 0x0FU);

    if (count > BSP_TOUCH_MAX_POINTS) {
        count = 0;                              /* 异常值：按"无触摸"处理 */
    }

    out->count = count;
    out->pressed = (count > 0U);

    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[CHSC_POINT_BASE_OFFSET + (size_t)i * CHSC_POINT_STRIDE];
        const uint8_t hi = p[3];                /* buf[5+5i]：高 4 位是 A 的高 8 位，低 4 位是 B 的高 8 位 */

        /* 这里给的是协议原始的两个 12bit 量 A/B，方向语义由 bsp_touch_read() 决定 */
        out->point[i].x = (uint16_t)(((uint16_t)(hi & 0xF0U) << 4) | p[1]);   /* A = ((hi>>4)<<8) | buf[3+5i] */
        out->point[i].y = (uint16_t)(((uint16_t)(hi & 0x0FU) << 8) | p[0]);   /* B = ((hi&0x0F)<<8) | buf[2+5i] */
    }

    return ESP_OK;
}

esp_err_t bsp_touch_read(bsp_touch_state_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_touch_state_t raw;
    esp_err_t err = bsp_touch_read_raw(&raw);
    if (err != ESP_OK) {
        return err;
    }

#if BSP_TOUCH_LANDSCAPE
    /* 横屏（与厂商驱动的横屏公式逐字一致）：
     *   x = A,  y = 屏高 - B
     */
    out->count = raw.count;
    out->pressed = raw.pressed;

    for (uint8_t i = 0; i < raw.count; i++) {
        const uint16_t a = raw.point[i].x;
        const uint16_t b = raw.point[i].y;

        out->point[i].x = a;
        out->point[i].y = (b <= BSP_LCD_HEIGHT) ? (uint16_t)(BSP_LCD_HEIGHT - b) : 0U;
    }
#else
    /* 竖屏：
     *   x = B,  y = A
     */
    out->count = raw.count;
    out->pressed = raw.pressed;

    for (uint8_t i = 0; i < raw.count; i++) {
        out->point[i].x = raw.point[i].y;
        out->point[i].y = raw.point[i].x;
    }
#endif

    return ESP_OK;
}

esp_err_t bsp_touch_read_pid(uint32_t *pid)
{
    if (s_dev == NULL || pid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[4] = {0};
    esp_err_t err = chsc_rd_reg(CHSC_REG_PID, buf, sizeof(buf));
    if (err == ESP_OK) {
        *pid = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    }
    return err;
}

bool bsp_touch_irq_asserted(void)
{
    return !bsp_xl9555_pin_read(BSP_XL9555_PIN_CTP_INT);
}
