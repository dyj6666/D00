/* ================================================================
 * audio.h —— I2S 音频（编解码/功放 + 数字麦克风）
 *
 * 硬件有 V1.0 / V1.1 两个版本，音频通路不同：
 *   V1.0：NS4168（I2S 数字功放，无需 I2C 配置）
 *   V1.1：ES8311（I2C 编解码，地址 0x18）+ NS4150B 功放
 * 本驱动**自动识别**：先探测 I2C 0x18，有应答就按 ES8311 初始化，
 * 否则按 NS4168（纯 I2S）处理。两条路都不需要改代码。
 *
 * 时钟说明：板上没有 MCLK，ES8311 用 BCLK 当 MCLK（厂商例程同款做法），
 * 因此 I2S 必须是 16bit 立体声（一帧 32bit），BCLK = 采样率 × 32。
 * 受此约束，ES8311 支持的采样率为 32k/44.1k/48k/64k/88.2k/96kHz。
 *
 * 扬声器使能（SPK_EN）挂在 XL9555 P0_5 上，由本模块统一管理。
 * ================================================================ */
#ifndef __BSP_AUDIO_H
#define __BSP_AUDIO_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 音频编解码类型（运行时自动识别） */
typedef enum
{
    BSP_AUDIO_CODEC_NONE = 0,   /*!< 未识别到 I2C 编解码（按纯 I2S 功放处理） */
    BSP_AUDIO_CODEC_ES8311,     /*!< ES8311（V1.1 板） */
    BSP_AUDIO_CODEC_NS4168,     /*!< NS4168（V1.0 板，纯 I2S） */
} bsp_audio_codec_t;

/** 音频参数 */
typedef struct
{
    uint32_t sample_rate_hz;    /*!< 采样率，默认 44100 */
    uint8_t  bits;              /*!< 位宽，固定 16（见文件头说明） */
    uint8_t  channels;          /*!< 声道数，固定 2 */
} bsp_audio_cfg_t;

/** 默认参数：44.1kHz / 16bit / 立体声 */
#define BSP_AUDIO_CFG_DEFAULT   { 44100U, 16U, 2U }

/**
 * @brief  覆盖默认参数（必须在 bsp_audio_init 之前调用）
 */
esp_err_t bsp_audio_configure(const bsp_audio_cfg_t *cfg);

/**
 * @brief  初始化音频：I2S 全双工 -> 识别并初始化编解码 -> 开功放
 */
esp_err_t bsp_audio_init(void);

/**
 * @brief  反初始化：静音、关功放、释放 I2S
 */
esp_err_t bsp_audio_deinit(void);

/**
 * @brief  查询识别到的编解码类型
 */
bsp_audio_codec_t bsp_audio_codec(void);

/**
 * @brief  查询是否已初始化
 */
bool bsp_audio_is_ready(void);

/**
 * @brief  开/关板载功放（SPK_EN）
 */
esp_err_t bsp_audio_enable_speaker(bool on);

/**
 * @brief  设置播放音量（0~100）
 * @note   仅 ES8311 有效（写 DAC 音量寄存器）；NS4168 只有硬件固定增益，
 *         此时应通过软件缩放采样数据来控制音量。
 */
esp_err_t bsp_audio_set_volume(uint8_t percent);

/**
 * @brief  静音/取消静音
 */
esp_err_t bsp_audio_set_mute(bool mute);

/**
 * @brief  写音频数据（播放）
 * @param  data        PCM 数据（16bit 交错立体声）
 * @param  bytes      字节数
 * @param  timeout_ms 超时
 * @return ESP_OK（超时返回 ESP_ERR_TIMEOUT 并已写入部分数据）
 */
esp_err_t bsp_audio_write(const void *data, size_t bytes, uint32_t timeout_ms);

/**
 * @brief  读音频数据（录音）
 */
esp_err_t bsp_audio_read(void *buf, size_t bytes, uint32_t timeout_ms);

/**
 * @brief  清空接收方向（丢弃已有录音数据）
 */
esp_err_t bsp_audio_flush_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_AUDIO_H */
