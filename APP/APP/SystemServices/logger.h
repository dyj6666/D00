/* ================================================================
 * logger —— 日志系统：Printf 格式化 + 流缓冲 + 输出路由
 *
 * 架构位置：APP 服务层；全模块经 LOG_Printf 输出，Shell/TCP 控制台复用
 * 核心流程：LOG_Printf -> TX 流缓冲 -> LoggerTXTask -> BSP UART / 路由钩子
 * ================================================================ */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"

/** @brief 初始化日志流缓冲与 DMA 通道 */
void LOG_Init(void);

/** @brief 格式化日志输出（经当前路由钩子/默认串口） */
void LOG_Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/** @brief 原始串口输出：不经过路由钩子（供适配器底层使用） */
void LOG_WriteRaw(const char *s, uint16_t len);

/** 输出路由钩子：统一命令框架把 LOG_Printf 导到当前适配器（TCP 控制台等） */
typedef void (*log_sink_fn)(const char *s, uint16_t len);

/** @brief 设置输出路由；置 NULL 恢复默认串口输出 */
void LOG_SetSink(log_sink_fn fn);

/** @brief 获取 RX 流缓冲（Shell 命令输入通道） */
StreamBufferHandle_t LOG_GetRxStream(void);

/** @brief 日志发送任务主体（供 freertos.c 调用） */
void LoggerTXTaskFunction(void);

/** @brief TX DMA 超时自愈计数（供 sysmon 健康检查） */
uint32_t LOG_GetTxErrCount(void);

#endif /* LOGGER_H */
