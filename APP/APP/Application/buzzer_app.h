#ifndef BUZZER_APP_H
#define BUZZER_APP_H

#include <stdint.h>

/* ================================================================
 * 蜂鸣器应用层（非阻塞提示音服务）
 *   - Buzzer_Beep         ：单次蜂鸣（软定时器控制时长，调用不阻塞）
 *   - Buzzer_BeepPattern  ：多段提示音（双击/连响，Tmr Svc 驱动状态机）
 *   - Buzzer_Stop         ：立即停止
 *   - 事件反馈：按键短按(25ms)/长按(120ms) 提示音
 * ================================================================ */

void BuzzerApp_Init(void);
void Buzzer_Beep(uint16_t ms);
void Buzzer_BeepPattern(uint8_t count, uint16_t on_ms, uint16_t gap_ms);
void Buzzer_Stop(void);

#endif
