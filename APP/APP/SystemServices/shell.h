/* ================================================================
 * shell —— UART Shell 传输适配器接口
 *
 * 架构位置：APP 服务层；行编辑与命令分发入口
 * ================================================================ */
#ifndef SHELL_H
#define SHELL_H
#include <stdint.h>

void Shell_Init(void);
void Shell_ProcessChar(uint8_t ch);

/* 供 freertos.c 的任务函数调用 */
void ShellTaskFunction(void);
#endif
