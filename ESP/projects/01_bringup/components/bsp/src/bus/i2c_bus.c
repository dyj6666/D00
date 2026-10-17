/* ================================================================
 * i2c_bus.c —— I2C0 共享总线服务层
 *
 * 实现要点：
 *   1. 用可重入互斥锁保护"事务组"，普通单次读写也走它，避免驱动层
 *      自己再加锁造成重复劳动；
 *   2. 错误日志限流（前 3 次全打，之后每 64 次打一条），防止器件掉线时
 *      日志把 CPU 和串口一起拖死；
 *   3. 出错时立即返回原始错误码（ESP_ERR_TIMEOUT / ESP_FAIL /
 *      ESP_ERR_INVALID_STATE），由上层决定重试策略。
 * ================================================================ */
#include "bsp/bus/i2c_bus.h"

#include "bsp/bsp_pinmap.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BSP/I2C";

#define BSP_I2C_ERR_LOG_FIRST       3U      /* 前几次错误全部打印 */
#define BSP_I2C_ERR_LOG_PERIOD      64U     /* 之后每 N 次打印一条 */

static i2c_master_bus_handle_t s_bus = NULL;
static SemaphoreHandle_t       s_lock = NULL;
static bsp_i2c_stats_t         s_stats;
static uint32_t                s_err_count = 0;

/* ---------------- 内部工具 ---------------- */

/**
 * @brief  记录一次错误（带日志限流）
 */
static void i2c_note_error(esp_err_t err)
{
    s_stats.errors++;
    s_stats.last_error = err;
    s_err_count++;

    if (s_err_count <= BSP_I2C_ERR_LOG_FIRST || (s_err_count % BSP_I2C_ERR_LOG_PERIOD) == 0U) {
        ESP_LOGW(TAG, "i2c error: %s (累计 %u 次)", esp_err_to_name(err), (unsigned)s_err_count);
    }
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_i2c_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;                          /* 幂等 */
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    const i2c_master_bus_config_t cfg = {
        .i2c_port                  = BSP_I2C_PORT,
        .sda_io_num                = BSP_I2C_SDA_GPIO,
        .scl_io_num                = BSP_I2C_SCL_GPIO,
        .clk_source                = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt         = 7,          /* 典型值：过滤 < 7 个模块时钟的毛刺 */
        .flags.enable_internal_pullup = true,    /* 板上有 4.7k 外部上拉，这里再兜一层 */
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new master bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C0 ready (SDA=%d SCL=%d %ukHz)", (int)BSP_I2C_SDA_GPIO,
             (int)BSP_I2C_SCL_GPIO, (unsigned)(BSP_I2C_SPEED_HZ / 1000U));
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (s_bus == NULL) {
        return ESP_OK;
    }

    esp_err_t err = i2c_del_master_bus(s_bus);
    if (err == ESP_OK) {
        s_bus = NULL;
    } else {
        ESP_LOGE(TAG, "del bus failed: %s（可能仍有从设备未卸载）", esp_err_to_name(err));
    }
    return err;
}

/* ---------------- 设备挂载 ---------------- */

esp_err_t bsp_i2c_add_device(uint16_t addr, uint32_t scl_hz, bsp_i2c_dev_t *out_dev)
{
    if (s_bus == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = (scl_hz != 0U) ? scl_hz : BSP_I2C_SPEED_HZ,
    };

    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", (unsigned)addr, esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_i2c_rm_device(bsp_i2c_dev_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_bus_rm_device(dev);
}

/* ---------------- 事务 ---------------- */

esp_err_t bsp_i2c_write(bsp_i2c_dev_t dev, const uint8_t *buf, size_t len)
{
    if (dev == NULL || buf == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    if (bsp_i2c_lock(BSP_I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = i2c_master_transmit(dev, buf, len, (int)BSP_I2C_TIMEOUT_MS);
    (void)bsp_i2c_unlock();

    if (err == ESP_OK) {
        s_stats.tx++;
    } else {
        i2c_note_error(err);
    }
    return err;
}

esp_err_t bsp_i2c_read(bsp_i2c_dev_t dev, uint8_t *buf, size_t len)
{
    if (dev == NULL || buf == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    if (bsp_i2c_lock(BSP_I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = i2c_master_receive(dev, buf, len, (int)BSP_I2C_TIMEOUT_MS);
    (void)bsp_i2c_unlock();

    if (err == ESP_OK) {
        s_stats.rx++;
    } else {
        i2c_note_error(err);
    }
    return err;
}

esp_err_t bsp_i2c_write_read(bsp_i2c_dev_t dev,
                             const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len)
{
    if (dev == NULL || wr == NULL || rd == NULL || wr_len == 0U || rd_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    if (bsp_i2c_lock(BSP_I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = i2c_master_transmit_receive(dev, wr, wr_len, rd, rd_len, (int)BSP_I2C_TIMEOUT_MS);
    (void)bsp_i2c_unlock();

    if (err == ESP_OK) {
        s_stats.rx++;
    } else {
        i2c_note_error(err);
    }
    return err;
}

esp_err_t bsp_i2c_reg_write(bsp_i2c_dev_t dev, uint8_t reg, const uint8_t *val, size_t len)
{
    if (dev == NULL || val == NULL || len == 0U || len > 8U) {
        return ESP_ERR_INVALID_ARG;             /* 超过 8 字节请用 bsp_i2c_write 组包 */
    }

    uint8_t buf[9];
    buf[0] = reg;
    for (size_t i = 0; i < len; i++) {
        buf[i + 1U] = val[i];
    }
    return bsp_i2c_write(dev, buf, len + 1U);
}

esp_err_t bsp_i2c_reg_read(bsp_i2c_dev_t dev, uint8_t reg, uint8_t *val, size_t len)
{
    if (dev == NULL || val == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_i2c_write_read(dev, &reg, 1U, val, len);
}

esp_err_t bsp_i2c_reg_write_u8(bsp_i2c_dev_t dev, uint8_t reg, uint8_t val)
{
    return bsp_i2c_reg_write(dev, reg, &val, 1U);
}

esp_err_t bsp_i2c_reg_read_u8(bsp_i2c_dev_t dev, uint8_t reg, uint8_t *val)
{
    return bsp_i2c_reg_read(dev, reg, val, 1U);
}

bool bsp_i2c_probe(uint16_t addr, uint32_t timeout_ms)
{
    if (s_bus == NULL) {
        return false;
    }
    return i2c_master_probe(s_bus, addr, (int)timeout_ms) == ESP_OK;
}

/* ---------------- 统计 ---------------- */

void bsp_i2c_get_stats(bsp_i2c_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

void bsp_i2c_clear_stats(void)
{
    s_stats.tx = 0;
    s_stats.rx = 0;
    s_stats.errors = 0;
    s_stats.last_error = ESP_OK;
    s_err_count = 0;
}

/* ---------------- 锁 ---------------- */

esp_err_t bsp_i2c_lock(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "i2c bus lock timeout(%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bsp_i2c_unlock(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreGiveRecursive(s_lock) == pdTRUE) ? ESP_OK : ESP_FAIL;
}
