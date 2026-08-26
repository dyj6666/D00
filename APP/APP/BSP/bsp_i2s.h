/* ================================================================
 * bsp_i2s —— I2S2 音频接口驱动（探索者 V3 板载，驱动 ES8388 Codec）
 *
 * 架构位置：APP BSP 层；音频服务（audio_svc）依赖。
 *
 * 引脚（探索者 V3 原理图/官方例程确认）：
 *   WS(LRCK)=PB12(AF5)  SCK=PB13(AF5)  SDOUT=PC2(AF6-I2S2ext)
 *   SDIN=PC3(AF5)       MCK=PC6(AF5)
 * TX DMA：DMA1_Stream4/CH0（半字，循环，双缓冲 + TC 中断）
 *
 * 采样率：48kHz 默认（PLLI2S 动态配置，查表法，见 I2S_PSC_TBL）。
 * ================================================================ */
#ifndef BSP_I2S_H
#define BSP_I2S_H

#include <stdint.h>

/* 初始化 I2S2（主机 TX）+ TX DMA 双缓冲；0=成功，1=采样率不支持 */
uint8_t BSP_I2S_Init(uint32_t samplerate);

/* 动态切换采样率（PLLI2S 重配）；0=成功 */
uint8_t BSP_I2S_SetSampleRate(uint32_t samplerate);

/* 启动双缓冲循环播放（buf0/buf1 各 num 个 16 位样本，num ≤ 65535） */
void BSP_I2S_Play(uint16_t *buf0, uint16_t *buf1, uint16_t num);

/* 停止播放（DMA 停止，I2S 保持使能） */
void BSP_I2S_Stop(void);

/* 注册 DMA 传输完成回调（每半缓冲触发一次，用于填充下一段波形） */
void BSP_I2S_SetCallback(void (*cb)(void));

/* 当前 DMA 正在读的缓冲索引（0/1，供回调填充另一块） */
uint8_t BSP_I2S_GetCurrentBuf(void);

#endif /* BSP_I2S_H */
