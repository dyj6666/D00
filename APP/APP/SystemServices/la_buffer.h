/* ================================================================
 * la_buffer —— 逻辑分析仪环形缓冲：写入/导出
 *
 * 架构位置：APP 服务层；采样数据落地与 HOSTLINK 导出
 * ================================================================ */
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
