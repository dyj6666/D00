/* ================================================================
 * audio_svc —— 音频服务：合成音调/音效播放（I2S2 + ES8388 → 板载喇叭）
 *
 * 架构位置：APP 服务层；BSP 依赖 bsp_i2s / bsp_es8388。
 *
 * 能力（第一版）：
 *   - 正弦音合成（任意频率，48kHz/16bit/双声道，相位累加 + 查表）
 *   - 非阻塞播放：请求入队，常驻任务执行，到时自动停止
 *   - 波形缓冲分配自外部 SRAM 池（ExtMem_Alloc，376KB 统一池）
 *
 * 后续扩展：WAV 播放（数据源 OTA 推送到外部 Flash）、多音合成。
 * ================================================================ */
#ifndef AUDIO_SVC_H
#define AUDIO_SVC_H

#include <stdint.h>

/* 模块初始化（module.c 注册，priority=6）：I2S2 + ES8388 + 播放任务 */
void Audio_Init(void);

/* 非阻塞播放正弦音（freq 1~20000Hz；duration_ms=0 表示持续播放直到
 * Audio_Stop；播放中调用则替换） */
void Audio_PlayTone(uint32_t freq_hz, uint32_t duration_ms);

/* 非阻塞播放 WAV（PCM 16bit，8k/11.025k/16k/22.05k/32k/44.1k/48k；
 * 采样率自动切换，播放结束恢复 48kHz；wav_len=0 或解析失败忽略） */
void Audio_PlayWav(const uint8_t *wav_data, uint32_t wav_len);

/* 立即停止播放 */
void Audio_Stop(void);

/* 播放状态：1=播放中 */
uint8_t Audio_IsPlaying(void);

/* 喇叭音量 0~33（0 最响，33 静音；ES8388 反向刻度） */
void Audio_SetVolume(uint8_t volume);

/* 音频状态快照（GUI 页直读） */
typedef struct {
    uint8_t  playing;        /* 1=播放中 */
    uint8_t  src;            /* 0=none 1=tone 2=wav */
    uint32_t freq_hz;        /* 当前 tone 频率（播放中/最近一次） */
    uint8_t  volume;         /* 喇叭音量 0~33 */
} audio_state_t;

void Audio_GetState(audio_state_t *st);

#endif /* AUDIO_SVC_H */
