/* ================================================================
 * adc.c —— 扩展排针 ADC（ESP32-S3 ADC1 单次转换）
 *
 * 说明：ADC_ATTEN_DB_12（旧名 DB_11）对应约 0~3.1V 输入范围，
 *       配合曲线拟合校准可以拿到 ±10mV 级别的读数精度。
 *       注意 GPIO5/6/8 同时也是扩展排针，接线前确认没有别的驱动占用。
 * ================================================================ */
#include "bsp/adc.h"

#include "bsp/bsp_pinmap.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "BSP/ADC";

typedef struct
{
    adc_channel_t channel;      /*!< ADC1 通道号 */
    gpio_num_t    gpio;         /*!< 对应引脚（仅供日志/文档） */
} adc_map_t;

static const adc_map_t s_map[BSP_ADC_CH_COUNT] = {
    { (adc_channel_t)BSP_ADC_CH_IO5, BSP_EXP_IO_5 },
    { (adc_channel_t)BSP_ADC_CH_IO6, BSP_EXP_IO_6 },
    { (adc_channel_t)BSP_ADC_CH_IO8, BSP_EXP_IO_8 },
};

static adc_oneshot_unit_handle_t s_unit = NULL;
static adc_cali_handle_t         s_cali = NULL;
static bool                      s_ready = false;

esp_err_t bsp_adc_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ① ADC1 单元 */
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new unit failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ② 逐通道配置（12bit + 12dB 衰减，约 0~3.1V） */
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    for (int i = 0; i < BSP_ADC_CH_COUNT; i++) {
        err = adc_oneshot_config_channel(s_unit, s_map[i].channel, &chan_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config channel %d failed: %s", (int)s_map[i].channel,
                     esp_err_to_name(err));
            adc_oneshot_del_unit(s_unit);
            s_unit = NULL;
            return err;
        }
    }

    /* ③ 校准（曲线拟合）。失败不致命：还能读原始值 */
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibration unavailable: %s（只提供原始值）", esp_err_to_name(err));
        s_cali = NULL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (IO%d/IO%d/IO%d, 12bit + %s)",
             (int)BSP_EXP_IO_5, (int)BSP_EXP_IO_6, (int)BSP_EXP_IO_8,
             (s_cali != NULL) ? "校准" : "未校准");
    return ESP_OK;
}

esp_err_t bsp_adc_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    if (s_cali != NULL) {
        (void)adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_unit != NULL) {
        (void)adc_oneshot_del_unit(s_unit);
        s_unit = NULL;
    }

    s_ready = false;
    return ESP_OK;
}

esp_err_t bsp_adc_read_raw(bsp_adc_ch_t ch, int *raw)
{
    if (!s_ready || raw == NULL || ch >= BSP_ADC_CH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return adc_oneshot_read(s_unit, s_map[ch].channel, raw);
}

esp_err_t bsp_adc_read_mv(bsp_adc_ch_t ch, int *mv)
{
    if (s_cali == NULL || mv == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int raw = 0;
    esp_err_t err = bsp_adc_read_raw(ch, &raw);
    if (err != ESP_OK) {
        return err;
    }
    return adc_cali_raw_to_voltage(s_cali, raw, mv);
}

esp_err_t bsp_adc_read_mv_avg(bsp_adc_ch_t ch, uint8_t times, int *mv)
{
    if (mv == NULL || times == 0U || times > 64U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cali == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t sum = 0;

    for (uint8_t i = 0; i < times; i++) {
        int one = 0;
        esp_err_t err = bsp_adc_read_mv(ch, &one);
        if (err != ESP_OK) {
            return err;
        }
        sum += one;
    }

    *mv = (int)(sum / (int64_t)times);
    return ESP_OK;
}
