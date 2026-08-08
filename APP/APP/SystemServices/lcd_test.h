#ifndef LCD_TEST_H
#define LCD_TEST_H

#include <stdint.h>

/* ================================================================
 * LCD 自动化测试（打基础用回归保障）
 *   - SelfTest ：GRAM 读回完整性 + 窗口边界 + 像素级文字渲染校验
 *   - Soak     ：驱动长稳（混合负载循环 + 周期读回校验 + 堆稳定性）
 *   - Stress   ：框架切页压力（快速命令注入 + 队列排空 + 丢命令统计）
 * SelfTest/Soak 须经 LcdUI_RunTest 在渲染任务内执行（串行绘制）；
 * Stress 在 Shell 任务上下文执行（命令注入 + 轮询排空）。
 * ================================================================ */

void LcdTest_RunSelfTest(void);
void LcdTest_RunSoak(uint16_t seconds);
void LcdTest_RunStress(uint16_t count);

#endif
