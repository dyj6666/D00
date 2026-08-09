#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"

void LOG_Init(void);
void LOG_Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* 原始串口输出（不经过路由钩子） */
void LOG_WriteRaw(const char *s, uint16_t len);

/* 输出路由钩子：统一命令框架用它把 LOG_Printf 导到当前适配器
 * （TCP 控制台等）。置 NULL 恢复默认串口输出。 */
typedef void (*log_sink_fn)(const char *s, uint16_t len);
void LOG_SetSink(log_sink_fn fn);

StreamBufferHandle_t LOG_GetRxStream(void);

/* 供 freertos.c 的任务函数调用 */
void LoggerTXTaskFunction(void);

#endif
