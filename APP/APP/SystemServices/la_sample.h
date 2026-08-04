#ifndef LA_SAMPLE_H
#define LA_SAMPLE_H

#include "la_config.h"

void LA_Sample_Init(void);
void LA_Sample_Start(LA_SampleMode mode);
void LA_Sample_Stop(void);

uint64_t LA_Sample_GetTimestamp(void);
uint8_t LA_Sample_GetChannelStates(void);
void LA_Timestamp_Overflow_Handler(void);

/* DMA 流模式（TIM1 + DMA2，硬件采样） */
void LA_Sample_Start_DMA(uint32_t sample_rate_hz);
uint32_t LA_Sample_Stop_DMA(void);      /* 返回本次累计样本数 */
uint32_t LA_Sample_GetDMACount(void);
uint8_t  LA_Sample_GetDMAOverrun(void);
uint32_t LA_Sample_GetDMABufferSize(void);
int      LA_Sample_SetDMABuffer(uint8_t use_sram);
uint8_t  LA_Sample_IsDMASram(void);
void LA_Sample_ReadDMABuffer(uint32_t *buf, uint32_t start, uint32_t count);

/* 采样状态变量（shell / 上位机变量注册使用） */
extern uint32_t la_samples;
extern uint32_t la_ch0_state;
extern uint32_t la_ch4_state;

/* shell 诊断用：打印 EXTI4/NVIC 状态 */
void LA_Diag_PrintExtiStatus(void);

#endif
