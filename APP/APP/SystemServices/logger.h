#ifndef LOGGER_H
#define LOGGER_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"

void LOG_Init(void);
void LOG_Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

StreamBufferHandle_t LOG_GetRxStream(void);

/* 供 freertos.c 的任务函数调用 */
void LoggerTXTaskFunction(void);

/* 供中断调用 */
void LOG_RxIdleCallback(uint16_t size);
void LOG_TxCpltCallback(void);
#endif
