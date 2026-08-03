/* 逻辑分析仪 SRAM 环形缓冲管理接口 */
#ifndef LA_BUFFER_H
#define LA_BUFFER_H

#include "la_config.h"

void LA_Buffer_Init(void);
void LA_Buffer_Write(uint64_t timestamp, uint8_t states);
uint32_t LA_Buffer_GetCount(void);
void LA_Buffer_Reset(void);
void LA_Buffer_Read(LA_SamplePoint *buf, uint32_t start, uint32_t count);
uint8_t LA_Buffer_IsSramOk(void);

#endif
