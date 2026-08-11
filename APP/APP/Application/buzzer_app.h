/* ================================================================
 * buzzer_app —— 蜂鸣器应用：提示音/告警
 *
 * 架构位置：APP 应用层；交互反馈
 * ================================================================ */
#ifndef BUZZER_APP_H
#define BUZZER_APP_H

#include <stdint.h>

/* ================================================================
 * 蜂鸣器应用层（非阻塞提示音服务）
 *   - Buzzer_Beep         ：单次蜂鸣（软定时器控制时长，调用不阻塞）
 *   - Buzzer_BeepPattern  ：多段提示音（双击/连响，Tmr Svc 驱动状态机）
 *   - Buzzer_PlaySequence ：任意节奏序列（各段响/停时长不同）
 *   - Buzzer_Ota*         ：OTA 专属旋律（开始/成功/失败）
 *   - Buzzer_Stop         ：立即停止
 *   - 事件反馈：按键短按(25ms)/长按(120ms) 提示音
 * ================================================================ */

void BuzzerApp_Init(void);
void Buzzer_Beep(uint16_t ms);
void Buzzer_BeepPattern(uint8_t count, uint16_t on_ms, uint16_t gap_ms);
void Buzzer_PlaySequence(const uint16_t *on_gap, uint8_t n);
void Buzzer_Stop(void);

/* ---------- OTA 旋律（有源蜂鸣器，节奏即音高） ---------- */
void Buzzer_OtaStart(void);    /* 滴-滴-嘟    ：升级开始 */
void Buzzer_OtaSuccess(void);  /* 滴-滴-滴-嘟  ：升级完成（新固件启动确认） */
void Buzzer_OtaFail(void);     /* 滴-滴-滴    ：升级失败 */

#endif
