// ymodem_port.h
#ifndef YMODEM_PORT_H
#define YMODEM_PORT_H

#include <stdint.h>
#include "fifo.h"

void     ymodem_port_init(void);                 // 初始化 UART，启动 RXNE 中断
void     ymodem_send_byte(uint8_t byte);         // 发送单字节（阻塞）
int32_t  ymodem_read_byte(uint32_t timeout_ms);  // 从 FIFO 取一字节，超时返回 -1
void     ymodem_feed_watchdog(void);             // 喂狗
uint32_t ymodem_get_tick(void);                  // 获取系统毫秒时间
void ymodem_uart_isr_handler(void);

#endif



