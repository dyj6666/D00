/* ================================================================
 * eeprom.c —— AT24C02 EEPROM 驱动
 *
 * AT24C02 时序要点：
 *   - 地址只有 1 字节（0~255），器件地址固定 0x50（A0~A2 接地）；
 *   - 写操作有"页边界"：一次连续写不能跨越 8 字节页，否则地址在页内回卷；
 *   - 每次写后有 5ms 内部写周期，期间不响应任何 I2C（正是我们用来 ACK 轮询的依据）。
 * ================================================================ */
#include "bsp/eeprom.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/EEPROM";

#define EEPROM_WRITE_CYCLE_MS       5U
#define EEPROM_ACK_POLL_TIMEOUT_MS  50U
#define EEPROM_ACK_POLL_INTERVAL_MS 1U

static bsp_i2c_dev_t s_dev = NULL;

/* ---------------- 内部 ---------------- */

/**
 * @brief  等待器件结束内部写周期（ACK 轮询）
 */
static esp_err_t eeprom_wait_ready(void)
{
    const int64_t deadline = (int64_t)EEPROM_ACK_POLL_TIMEOUT_MS;
    int64_t waited = 0;

    while (waited < deadline) {
        if (bsp_i2c_probe(BSP_EEPROM_I2C_ADDR, EEPROM_ACK_POLL_INTERVAL_MS)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(EEPROM_ACK_POLL_INTERVAL_MS));
        waited += EEPROM_ACK_POLL_INTERVAL_MS;
    }

    ESP_LOGE(TAG, "write cycle timeout (%u ms)", (unsigned)EEPROM_ACK_POLL_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_eeprom_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_add_device(BSP_EEPROM_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    if (!bsp_i2c_probe(BSP_EEPROM_I2C_ADDR, EEPROM_ACK_POLL_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "device not found at 0x%02X", BSP_EEPROM_I2C_ADDR);
        (void)bsp_i2c_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "ready (AT24C02 %u bytes)", (unsigned)BSP_EEPROM_SIZE);
    return ESP_OK;
}

esp_err_t bsp_eeprom_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }
    esp_err_t err = bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

/* ---------------- 读写 ---------------- */

esp_err_t bsp_eeprom_read(uint16_t addr, void *buf, size_t len)
{
    if (s_dev == NULL || buf == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)addr + len > BSP_EEPROM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t mem = (uint8_t)addr;
    return bsp_i2c_write_read(s_dev, &mem, 1U, (uint8_t *)buf, len);
}

esp_err_t bsp_eeprom_write(uint16_t addr, const void *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)addr + len > BSP_EEPROM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint16_t pos = addr;
    size_t left = len;

    /* 整个写入过程持锁：避免别的任务在页间插入 I2C 事务 */
    esp_err_t err = bsp_i2c_lock(BSP_I2C_TIMEOUT_MS * 4U);
    if (err != ESP_OK) {
        return err;
    }

    while (left > 0U) {
        /* 本页还能写多少字节 */
        const uint16_t page_off = (uint16_t)(pos % BSP_EEPROM_PAGE_SIZE);
        size_t chunk = BSP_EEPROM_PAGE_SIZE - page_off;

        if (chunk > left) {
            chunk = left;
        }

        uint8_t buf[1U + BSP_EEPROM_PAGE_SIZE];
        buf[0] = (uint8_t)pos;
        for (size_t i = 0; i < chunk; i++) {
            buf[1U + i] = src[i];
        }

        err = bsp_i2c_write(s_dev, buf, chunk + 1U);
        if (err != ESP_OK) {
            break;
        }

        err = eeprom_wait_ready();
        if (err != ESP_OK) {
            break;
        }

        src += chunk;
        pos = (uint16_t)(pos + chunk);
        left -= chunk;
    }

    (void)bsp_i2c_unlock();
    return err;
}

esp_err_t bsp_eeprom_read_u8(uint16_t addr, uint8_t *val)
{
    return bsp_eeprom_read(addr, val, 1U);
}

esp_err_t bsp_eeprom_write_u8(uint16_t addr, uint8_t val)
{
    return bsp_eeprom_write(addr, &val, 1U);
}

/* ---------------- 自检与擦除 ---------------- */

esp_err_t bsp_eeprom_self_test(uint16_t addr)
{
    uint8_t original = 0;
    uint8_t pattern[4] = {0x5AU, 0xA5U, 0x3CU, 0xC3U};
    uint8_t readback[4] = {0};
    esp_err_t err;

    err = bsp_eeprom_read_u8(addr, &original);
    if (err != ESP_OK) {
        return err;
    }

    err = bsp_eeprom_write(addr, pattern, sizeof(pattern));
    if (err != ESP_OK) {
        return err;
    }

    err = bsp_eeprom_read(addr, readback, sizeof(readback));
    if (err == ESP_OK) {
        for (size_t i = 0; i < sizeof(pattern); i++) {
            if (readback[i] != pattern[i]) {
                ESP_LOGE(TAG, "self test mismatch at +%u: 0x%02X != 0x%02X",
                         (unsigned)i, (unsigned)readback[i], (unsigned)pattern[i]);
                err = ESP_FAIL;
                break;
            }
        }
    }

    (void)bsp_eeprom_write_u8(addr, original);  /* 恢复原值 */
    return err;
}

esp_err_t bsp_eeprom_erase(void)
{
    uint8_t page[BSP_EEPROM_PAGE_SIZE];

    for (size_t i = 0; i < sizeof(page); i++) {
        page[i] = 0xFFU;
    }

    for (uint16_t addr = 0; addr < BSP_EEPROM_SIZE; addr = (uint16_t)(addr + BSP_EEPROM_PAGE_SIZE)) {
        esp_err_t err = bsp_eeprom_write(addr, page, sizeof(page));
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
