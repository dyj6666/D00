/* ================================================================
 * uart.c —— 外部串口（备用 UART）
 *
 * 资源与淘汰策略：
 *   - 接收缓冲 2KB + 事件队列 16 项，足够应付 115200 下的突发数据；
 *   - 事件队列溢出时 uart 驱动会丢数据并报告 UART_BUFFER_FULL，
 *     所以队列长度留了余量，业务侧尽快取走即可。
 * ================================================================ */
#include "bsp/uart.h"

#include "bsp/bsp_pinmap.h"

#include <string.h>

static const char *TAG = "BSP/UART";

static bsp_uart_cfg_t s_cfg = BSP_UART_CFG_DEFAULT;
static QueueHandle_t  s_queue = NULL;
static bool           s_ready = false;

esp_err_t bsp_uart_configure(const bsp_uart_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    return ESP_OK;
}

esp_err_t bsp_uart_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const uart_config_t line_cfg = {
        .baud_rate  = (int)s_cfg.baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_cfg.port, (int)s_cfg.rx_buf_size,
                                        (int)s_cfg.tx_buf_size,
                                        (int)s_cfg.event_queue_len, &s_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(s_cfg.port, &line_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "param config failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = uart_set_pin(s_cfg.port, (int)s_cfg.tx_gpio, (int)s_cfg.rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set pin failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (UART%d %u bps, TX=IO%d RX=IO%d)",
             (int)s_cfg.port, (unsigned)s_cfg.baud_rate,
             (int)s_cfg.tx_gpio, (int)s_cfg.rx_gpio);
    return ESP_OK;

fail:
    (void)uart_driver_delete(s_cfg.port);
    s_queue = NULL;
    return err;
}

esp_err_t bsp_uart_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    esp_err_t err = uart_driver_delete(s_cfg.port);
    s_queue = NULL;
    s_ready = false;
    return err;
}

int bsp_uart_write(const void *data, size_t len, uint32_t timeout_ms)
{
    if (!s_ready || data == NULL || len == 0U) {
        return -1;
    }

    if (timeout_ms == 0U) {
        return uart_tx_chars(s_cfg.port, (const char *)data, (uint32_t)len);
    }
    return uart_write_bytes(s_cfg.port, data, len);
}

int bsp_uart_read(void *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_ready || buf == NULL || len == 0U) {
        return -1;
    }
    return uart_read_bytes(s_cfg.port, buf, (uint32_t)len, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t bsp_uart_flush(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_flush_input(s_cfg.port);
}

QueueHandle_t bsp_uart_event_queue(void)
{
    return s_queue;
}

esp_err_t bsp_uart_set_baudrate(uint32_t baud_rate)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uart_set_baudrate(s_cfg.port, baud_rate);
    if (err == ESP_OK) {
        s_cfg.baud_rate = baud_rate;
    }
    return err;
}
