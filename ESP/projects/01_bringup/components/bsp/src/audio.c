/* ================================================================
 * audio.c —— I2S 音频 + ES8311 / NS4168 自动识别
 *
 * ES8311 初始化序列完全按厂商 v5.4 例程的寄存器写入顺序移植
 * （该序列来自 Espressif 官方 es8311 驱动派生版本），只做了两处收敛：
 *   1. 时钟系数表按"BCLK 当 MCLK"这一固定用法精简：pre_div=1、pre_multi=8、
 *      adc_div=dac_div=1、fs_mode=0、osr=0x10，只有 lrck_l 与 bclk_div
 *      随采样率变化（96k 例外）；
 *   2. 每个寄存器写入都检查返回值，出错立即退出，便于定位（厂商是 |= 累积）。
 *
 * 顺序要点：**必须先让 I2S 出时钟，再配置 ES8311**。
 * 因为 ES8311 的 MCLK 来自 BCLK，没有时钟它内部状态机不工作。
 * ================================================================ */
#include "bsp/audio.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"
#include "bsp/xl9555.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/AUDIO";

/* ---------------- I2S 参数 ---------------- */

#define AUDIO_I2S_DMA_DESC      6U
#define AUDIO_I2S_DMA_FRAMES    240U

/* ---------------- ES8311 寄存器 ---------------- */

#define ES8311_REG00            0x00U   /* 复位 */
#define ES8311_REG01            0x01U   /* 时钟源/使能 */
#define ES8311_REG02            0x02U   /* 时钟分频/倍频 */
#define ES8311_REG03            0x03U   /* ADC 帧同步/过采样 */
#define ES8311_REG04            0x04U   /* DAC 过采样 */
#define ES8311_REG05            0x05U   /* ADC/DAC 分频 */
#define ES8311_REG06            0x06U   /* BCLK 反相/分频 */
#define ES8311_REG07            0x07U   /* LRCK 分频高 */
#define ES8311_REG08            0x08U   /* LRCK 分频低 */
#define ES8311_REG09            0x09U   /* DAC 串行口 */
#define ES8311_REG0A            0x0AU   /* ADC 串行口 */
#define ES8311_REG0B            0x0BU
#define ES8311_REG0C            0x0CU
#define ES8311_REG0D            0x0DU
#define ES8311_REG0E            0x0EU
#define ES8311_REG10            0x10U
#define ES8311_REG11            0x11U
#define ES8311_REG12            0x12U
#define ES8311_REG13            0x13U
#define ES8311_REG14            0x14U   /* 数字/模拟麦克风 + PGA 增益 */
#define ES8311_REG15            0x15U
#define ES8311_REG16            0x16U   /* ADC 配置（含 MIC 增益） */
#define ES8311_REG17            0x17U   /* ADC 音量 */
#define ES8311_REG1B            0x1BU
#define ES8311_REG1C            0x1CU
#define ES8311_REG31            0x31U   /* DAC 静音 */
#define ES8311_REG32            0x32U   /* DAC 音量 */
#define ES8311_REG37            0x37U
#define ES8311_REG44            0x44U
#define ES8311_REG45            0x45U

#define ES8311_RESET_DELAY_MS   80U
#define ES8311_VOL_MAX          100U

/** 每个采样率的 LRCK 分频低字节与 BCLK 分频（其余系数固定） */
typedef struct
{
    uint32_t rate;
    uint8_t  lrck_l;
    uint8_t  bclk_div;
} es8311_clk_t;

static const es8311_clk_t s_es_clk[] = {
    { 32000U, 0xFFU, 4U },
    { 44100U, 0xFFU, 4U },
    { 48000U, 0xFFU, 4U },
    { 64000U, 0xFFU, 4U },
    { 88200U, 0xFFU, 4U },
    { 96000U, 0x7FU, 2U },
};

/* ---------------- 状态 ---------------- */

static bsp_audio_cfg_t    s_cfg = BSP_AUDIO_CFG_DEFAULT;
static i2s_chan_handle_t  s_tx = NULL;
static i2s_chan_handle_t  s_rx = NULL;
static bsp_i2c_dev_t      s_codec = NULL;
static bsp_audio_codec_t  s_codec_type = BSP_AUDIO_CODEC_NONE;
static bool               s_ready = false;
static bool               s_muted = false;
static uint8_t            s_volume = 60U;

/* ---------------- ES8311 底层 ---------------- */

/**
 * @brief  写 ES8311 寄存器
 */
static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    return bsp_i2c_reg_write_u8(s_codec, reg, val);
}

/**
 * @brief  读 ES8311 寄存器（读失败返回 0，调用方按位或即可）
 */
static uint8_t es_read(uint8_t reg)
{
    uint8_t val = 0;

    (void)bsp_i2c_reg_read_u8(s_codec, reg, &val);
    return val;
}

/**
 * @brief  查表得到当前采样率的 ES8311 时钟系数
 */
static const es8311_clk_t *es_find_clk(uint32_t rate)
{
    for (size_t i = 0; i < BSP_COUNT_OF(s_es_clk); i++) {
        if (s_es_clk[i].rate == rate) {
            return &s_es_clk[i];
        }
    }
    return NULL;
}

/**
 * @brief  ES8311 初始化（16bit、I2S 标准格式、BCLK 当 MCLK）
 * @note   调用前 I2S 必须已经在出时钟
 */
static esp_err_t es8311_init(uint32_t rate)
{
    const es8311_clk_t *clk = es_find_clk(rate);

    if (clk == NULL) {
        ESP_LOGE(TAG, "ES8311 不支持 %uHz（支持 32k/44.1k/48k/64k/88.2k/96k）",
                 (unsigned)rate);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    /* 顺序与厂商例程逐条一致 */
    err = es_write(ES8311_REG45, 0x00U);
    if (err == ESP_OK) { err = es_write(ES8311_REG01, 0x30U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG02, 0x00U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG03, 0x10U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG16, 0x24U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG04, 0x10U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG05, 0x00U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG0B, 0x00U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG0C, 0x00U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG10, 0x1FU); }
    if (err == ESP_OK) { err = es_write(ES8311_REG11, 0x7FU); }
    if (err == ESP_OK) { err = es_write(ES8311_REG00, 0x80U); }
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(ES8311_RESET_DELAY_MS));

    /* 从机模式（BCLK/LRCK 由 ESP32 提供） */
    uint8_t reg00 = (uint8_t)(es_read(ES8311_REG00) & ~0x40U);
    err = es_write(ES8311_REG00, reg00);
    if (err == ESP_OK) { err = es_write(ES8311_REG0D, 0x01U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG01, 0x3FU); }
    if (err != ESP_OK) {
        return err;
    }

    /* MCLK 源选择：用 SCLK(BCLK) 当 MCLK */
    err = es_write(ES8311_REG01, (uint8_t)(es_read(ES8311_REG01) | 0x80U));
    if (err != ESP_OK) {
        return err;
    }

    /* 时钟树：pre_div=1(->0), pre_multi=8(->datmp=3), adc_div=dac_div=1(->0) */
    const uint8_t reg02 = (uint8_t)((es_read(ES8311_REG02) & 0x07U) | (3U << 3));
    err = es_write(ES8311_REG02, reg02);
    if (err == ESP_OK) { err = es_write(ES8311_REG05, 0x00U); }
    /* ADC/DAC 过采样 0x10、fs_mode = 0 */
    if (err == ESP_OK) { err = es_write(ES8311_REG03, (uint8_t)(es_read(ES8311_REG03) & 0x80U) | 0x10U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG04, (uint8_t)(es_read(ES8311_REG04) & 0x80U) | 0x10U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG07, (uint8_t)(es_read(ES8311_REG07) & 0xC0U)); }
    if (err == ESP_OK) { err = es_write(ES8311_REG08, clk->lrck_l); }
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(ES8311_RESET_DELAY_MS));

    /* BCLK 分频（<19 时寄存器写 div-1，与厂商一致） */
    const uint8_t bclk_field = (clk->bclk_div < 19U) ? (uint8_t)(clk->bclk_div - 1U) : clk->bclk_div;
    const uint8_t reg06 = (uint8_t)((es_read(ES8311_REG06) & 0xE0U) | bclk_field);

    err = es_write(ES8311_REG06, reg06);
    if (err != ESP_OK) {
        return err;
    }

    /* 串行口：16bit + I2S 标准格式（低 2 位 = 0） */
    const uint8_t dac_iface = (uint8_t)((es_read(ES8311_REG09) & 0xC0U) | 0x0CU);
    const uint8_t adc_iface = (uint8_t)((es_read(ES8311_REG0A) & 0xC0U) | 0x0CU);

    err = es_write(ES8311_REG09, dac_iface);
    if (err == ESP_OK) { err = es_write(ES8311_REG0A, adc_iface); }

    /* 时钟极性：MCLK 不反相、SCLK 不反相 */
    if (err == ESP_OK) { err = es_write(ES8311_REG01, (uint8_t)(es_read(ES8311_REG01) & ~0x40U)); }
    if (err == ESP_OK) { err = es_write(ES8311_REG06, (uint8_t)(es_read(ES8311_REG06) & ~0x20U)); }

    /* 模拟麦克风（关 DMIC）、DAC/ADC 通路与增益 */
    if (err == ESP_OK) { err = es_write(ES8311_REG14, 0x1AU); }
    if (err == ESP_OK) { err = es_write(ES8311_REG14, (uint8_t)(es_read(ES8311_REG14) & ~0x40U)); }
    if (err == ESP_OK) { err = es_write(ES8311_REG12, 0x00U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG13, 0x10U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG0E, 0x02U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG15, 0x40U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG1B, 0x0AU); }
    if (err == ESP_OK) { err = es_write(ES8311_REG1C, 0x6AU); }
    if (err == ESP_OK) { err = es_write(ES8311_REG37, 0x48U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG44, 0x08U); }
    if (err == ESP_OK) { err = es_write(ES8311_REG17, 0xBFU); }     /* ADC 音量 */
    if (err == ESP_OK) { err = es_write(ES8311_REG32, 0xBFU); }     /* DAC 音量 */
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/* ---------------- 初始化辅助 ---------------- */

/**
 * @brief  建立 I2S 全双工通道并使其出时钟
 */
static esp_err_t audio_i2s_start(void)
{
    const i2s_chan_config_t chan_cfg = {
        .id             = (i2s_port_t)BSP_I2S_PORT,
        .role           = I2S_ROLE_MASTER,
        .dma_desc_num   = AUDIO_I2S_DMA_DESC,
        .dma_frame_num  = AUDIO_I2S_DMA_FRAMES,
        .auto_clear     = true,         /* 无数据时自动输出 0，避免喇叭噪声 */
    };

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new channel failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_cfg.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK_GPIO,
            .bclk = BSP_I2S_BCLK_GPIO,
            .ws   = BSP_I2S_WS_GPIO,
            .dout = BSP_I2S_DOUT_GPIO,
            .din  = BSP_I2S_DIN_GPIO,
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init std mode failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 先开时钟：ES8311 用 BCLK 当 MCLK，没有时钟它不工作 */
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief  识别并初始化编解码
 */
static esp_err_t audio_codec_probe(void)
{
    /* ① 有 I2C 应答 => ES8311（V1.1 板） */
    if (bsp_i2c_probe(BSP_ES8311_I2C_ADDR, BSP_I2C_TIMEOUT_MS)) {
        esp_err_t err = bsp_i2c_add_device(BSP_ES8311_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_codec);
        if (err != ESP_OK) {
            return err;
        }

        err = es8311_init(s_cfg.sample_rate_hz);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(err));
            (void)bsp_i2c_rm_device(s_codec);
            s_codec = NULL;
            return err;
        }

        s_codec_type = BSP_AUDIO_CODEC_ES8311;
        (void)bsp_audio_set_volume(s_volume);
        (void)bsp_audio_set_mute(false);
        return ESP_OK;
    }

    /* ② 无 I2C 应答 => NS4168（V1.0 板），只需 I2S + 功放使能 */
    s_codec_type = BSP_AUDIO_CODEC_NS4168;
    ESP_LOGW(TAG, "0x%02X 无应答，按 NS4168（纯 I2S 功放）处理", BSP_ES8311_I2C_ADDR);
    return ESP_OK;
}

/* ---------------- 对外接口 ---------------- */

esp_err_t bsp_audio_configure(const bsp_audio_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 位宽/声道固定：BCLK 必须是 采样率×32，否则 ES8311 时钟树对不上 */
    if (cfg->bits != 16U || cfg->channels != 2U) {
        ESP_LOGE(TAG, "仅支持 16bit 立体声（当前 %u bit / %u ch）",
                 (unsigned)cfg->bits, (unsigned)cfg->channels);
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = audio_i2s_start();
    if (err != ESP_OK) {
        goto fail;
    }

    err = audio_codec_probe();
    if (err != ESP_OK) {
        goto fail;
    }

    /* 最后开功放，避免上电 POP 声 */
    err = bsp_audio_enable_speaker(true);
    if (err != ESP_OK) {
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (%uHz 16bit stereo, codec=%s)",
             (unsigned)s_cfg.sample_rate_hz,
             (s_codec_type == BSP_AUDIO_CODEC_ES8311) ? "ES8311" : "NS4168/无");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    if (s_rx != NULL) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return err;
}

esp_err_t bsp_audio_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    (void)bsp_audio_set_mute(true);
    (void)bsp_audio_enable_speaker(false);

    if (s_codec != NULL) {
        (void)bsp_i2c_rm_device(s_codec);
        s_codec = NULL;
    }

    if (s_rx != NULL) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }

    s_codec_type = BSP_AUDIO_CODEC_NONE;
    s_ready = false;
    return ESP_OK;
}

bsp_audio_codec_t bsp_audio_codec(void)
{
    return s_codec_type;
}

bool bsp_audio_is_ready(void)
{
    return s_ready;
}

esp_err_t bsp_audio_enable_speaker(bool on)
{
    return bsp_xl9555_pin_write(BSP_XL9555_PIN_SPK_EN, on);
}

esp_err_t bsp_audio_set_volume(uint8_t percent)
{
    if (percent > ES8311_VOL_MAX) {
        percent = (uint8_t)ES8311_VOL_MAX;
    }
    s_volume = percent;

    if (s_codec_type != BSP_AUDIO_CODEC_ES8311 || s_codec == NULL) {
        return ESP_OK;              /* NS4168 无软件音量，交给上层缩放数据 */
    }

    /* 与厂商一致的线性映射：100% -> 0xFF */
    const uint8_t val = (uint8_t)((percent * 255U) / ES8311_VOL_MAX);

    return es_write(ES8311_REG32, val);
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    s_muted = mute;

    if (s_codec_type != BSP_AUDIO_CODEC_ES8311 || s_codec == NULL) {
        return ESP_OK;
    }

    /* 用 DAC 通路上电/掉电位实现静音（避免直接写 REG31 引起爆音） */
    uint8_t reg31 = es_read(ES8311_REG31);

    reg31 = mute ? (uint8_t)(reg31 | 0x60U) : (uint8_t)(reg31 & ~0x60U);
    return es_write(ES8311_REG31, reg31);
}

esp_err_t bsp_audio_write(const void *data, size_t bytes, uint32_t timeout_ms)
{
    if (!s_ready || s_tx == NULL || data == NULL || bytes == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, data, bytes, &written, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write timeout: %u/%u bytes", (unsigned)written, (unsigned)bytes);
    }
    return err;
}

esp_err_t bsp_audio_read(void *buf, size_t bytes, uint32_t timeout_ms)
{
    if (!s_ready || s_rx == NULL || buf == NULL || bytes == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t got = 0;
    return i2s_channel_read(s_rx, buf, bytes, &got, timeout_ms);
}

esp_err_t bsp_audio_flush_rx(void)
{
    if (!s_ready || s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 读空当前 DMA 里的残留数据（非阻塞方式轮询） */
    uint8_t scratch[256];

    for (int i = 0; i < 8; i++) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, scratch, sizeof(scratch), &got, 0) != ESP_OK || got == 0U) {
            break;
        }
    }
    return ESP_OK;
}
