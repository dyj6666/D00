/* ================================================================
 * spi_bus.c —— SPI2 共享总线服务层
 *
 * 参数选择说明：
 *   - max_transfer_sz 取 4KB：TF 卡单扇区 512B、一次多扇区读也就几 KB，
 *     没必要像 LCD 那样按整屏申请，省内部 RAM；
 *   - DMA 用 SPI_DMA_CH_AUTO，让驱动自动挑通道；
 *   - quadwp/quadhd 置 -1：本板 SPI 走普通 4 线，不用 QSPI 附加线。
 * ================================================================ */
#include "bsp/bus/spi_bus.h"

#include "bsp/bsp_pinmap.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BSP/SPI";

#define BSP_SPI_MAX_TRANSFER_SZ     (4096U)

static bool              s_bus_ready = false;
static SemaphoreHandle_t s_lock = NULL;

esp_err_t bsp_spi_bus_init(void)
{
    if (s_bus_ready) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    const spi_bus_config_t cfg = {
        .mosi_io_num     = BSP_SPI_MOSI_GPIO,
        .miso_io_num     = BSP_SPI_MISO_GPIO,
        .sclk_io_num     = BSP_SPI_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BSP_SPI_MAX_TRANSFER_SZ,
    };

    esp_err_t err = spi_bus_initialize((spi_host_device_t)BSP_SPI_HOST, &cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        s_bus_ready = true;                     /* 已被别的组件初始化，视作成功 */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_bus_ready = true;
    ESP_LOGI(TAG, "SPI%d ready (SCK=%d MISO=%d MOSI=%d CS=%d)", (int)BSP_SPI_HOST,
             (int)BSP_SPI_SCK_GPIO, (int)BSP_SPI_MISO_GPIO,
             (int)BSP_SPI_MOSI_GPIO, (int)BSP_SPI_TF_CS_GPIO);
    return ESP_OK;
}

esp_err_t bsp_spi_bus_deinit(void)
{
    if (!s_bus_ready) {
        return ESP_OK;
    }

    esp_err_t err = spi_bus_free((spi_host_device_t)BSP_SPI_HOST);
    if (err == ESP_OK) {
        s_bus_ready = false;
    } else {
        ESP_LOGW(TAG, "bus free failed: %s", esp_err_to_name(err));
    }
    return err;
}

spi_host_device_t bsp_spi_host(void)
{
    return (spi_host_device_t)BSP_SPI_HOST;
}

esp_err_t bsp_spi_add_device(const spi_device_interface_config_t *cfg, spi_device_handle_t *out_dev)
{
    if (!s_bus_ready || cfg == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return spi_bus_add_device((spi_host_device_t)BSP_SPI_HOST, cfg, out_dev);
}

esp_err_t bsp_spi_rm_device(spi_device_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return spi_bus_remove_device(dev);
}

esp_err_t bsp_spi_lock(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bsp_spi_unlock(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreGive(s_lock) == pdTRUE) ? ESP_OK : ESP_FAIL;
}
