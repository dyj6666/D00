/* 信号发生器服务：为逻辑分析仪等提供可观测的真实总线测试信号。
 *
 * 当前支持 USART6_TX（PC6）持续输出 ASCII 帧，供 LA 采样与上位机协议解码演示。
 * 注意：PC6 复用会从 TIM8_CH1(PWM) 切换为 USART6_TX，复位后恢复。
 */
#ifndef SIGNAL_GEN_H
#define SIGNAL_GEN_H

#include <stdint.h>

#define SG_TEXT_MAX   64
#define SG_BAUD_MIN   1200
#define SG_BAUD_MAX   921600

/* 启动 UART 测试帧发生器：后台任务按 interval_ms 间隔循环发送 text。
 * 成功返回 0；参数非法返回 -1，外设初始化失败返回 -2，任务创建失败返回 -3。 */
int    SG_UartStart(uint32_t baud, const char *text, uint16_t interval_ms);

/* 停止发生器（PC6 保持 USART6_TX 空闲高电平，复位后恢复 TIM8 PWM） */
void   SG_UartStop(void);

/* 查询发生器是否在运行 */
uint8_t SG_UartIsRunning(void);

#endif
