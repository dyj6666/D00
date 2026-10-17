/* ================================================================
 * temperature.c —— 片内温度传感器
 *
 * 量程取 -10~80℃：超出范围的读数会被驱动钳位，对普通工况足够，
 * 也避免配置成宽量程带来的精度损失。
 * ================================================================ */
#include "bsp/temperature.h"

#include "driver/temperature_sensor.h"

static const char *TAG = "BSP/TEMP";

#define TEMP_RANGE_MIN          (-10)
#define TEMP_RANGE_MAX          (80)

static temperature_sensor_handle_t s_tsens = NULL;

esp_err_t bsp_temperature_init(void)
{
    if (s_tsens != NULL) {
        return ESP_OK;
    }

    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(TEMP_RANGE_MIN,
                                                                             TEMP_RANGE_MAX);

    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install failed: %s", esp_err_to_name(err));
        s_tsens = NULL;
        return err;
    }

    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable failed: %s", esp_err_to_name(err));
        (void)temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return err;
    }

    ESP_LOGI(TAG, "ready (range %d~%d C)", TEMP_RANGE_MIN, TEMP_RANGE_MAX);
    return ESP_OK;
}

esp_err_t bsp_temperature_deinit(void)
{
    if (s_tsens == NULL) {
        return ESP_OK;
    }

    (void)temperature_sensor_disable(s_tsens);
    esp_err_t err = temperature_sensor_uninstall(s_tsens);
    s_tsens = NULL;
    return err;
}

esp_err_t bsp_temperature_read(float *celsius)
{
    if (s_tsens == NULL || celsius == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return temperature_sensor_get_celsius(s_tsens, celsius);
}

esp_err_t bsp_temperature_read_avg(uint8_t times, float *celsius)
{
    if (celsius == NULL || times == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    float sum = 0.0f;

    for (uint8_t i = 0; i < times; i++) {
        float one = 0.0f;
        esp_err_t err = bsp_temperature_read(&one);
        if (err != ESP_OK) {
            return err;
        }
        sum += one;
    }

    *celsius = sum / (float)times;
    return ESP_OK;
}
