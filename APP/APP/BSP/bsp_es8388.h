/* ================================================================
 * bsp_es8388 —— ES8388 音频 Codec 驱动（探索者 V3 板载）
 *
 * 架构位置：APP BSP 层；音频服务（audio_svc）依赖。
 *
 * 控制接口：软 I2C（PB8=SCL / PB9=SDA），与板载 AT24C02 **共用总线**
 *   （探索者原厂设计：24C02 地址 0xA0、ES8388 地址 0x10，分时访问）。
 *   本驱动自带互斥锁；ES8388 访问频率极低（初始化/音量），与 EEPROM
 *   的竞争窗口为微秒级事务，由各自锁 + 短临界区缓解。
 *
 * 音频通路：I2S2 数据 → ES8388 DAC → 板载喇叭（SPK_L/R，内置功放）
 *   与耳机座（HP）。喇叭音量寄存器 0x30/0x31（0~33）。
 * ================================================================ */
#ifndef BSP_ES8388_H
#define BSP_ES8388_H

#include <stdint.h>

/* 初始化 ES8388（软复位 + 寄存器默认配置）；0=成功 */
uint8_t BSP_ES8388_Init(void);

/* 写/读寄存器（内部持锁） */
uint8_t BSP_ES8388_WriteReg(uint8_t reg, uint8_t val);
uint8_t BSP_ES8388_ReadReg(uint8_t reg);

/* DAC/ADC 使能：dacen=1 开 DAC（播放），adcen=0 关 ADC（仅播放） */
void BSP_ES8388_AddaCfg(uint8_t dacen, uint8_t adcen);

/* DAC 输出通道：o1en=通道1（喇叭/耳机 L）、o2en=通道2（R） */
void BSP_ES8388_OutputCfg(uint8_t o1en, uint8_t o2en);

/* I2S 格式：fmt 0=Philips 3=PCM；len 3=16bit */
void BSP_ES8388_I2sCfg(uint8_t fmt, uint8_t len);

/* 喇叭音量 0~33（0 最响，33 静音；ES8388 反向刻度） */
void BSP_ES8388_SpkVolSet(uint8_t volume);

/* 耳机音量 0~33 */
void BSP_ES8388_HpVolSet(uint8_t volume);

#endif /* BSP_ES8388_H */
