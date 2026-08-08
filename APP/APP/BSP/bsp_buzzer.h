#ifndef BSP_BUZZER_H
#define BSP_BUZZER_H

/* ================================================================
 * 板载有源蜂鸣器 BSP（探索者V3：PF8，高电平发声，无需 PWM）
 * ================================================================ */

void BSP_Buzzer_Init(void);
void BSP_Buzzer_On(void);
void BSP_Buzzer_Off(void);
void BSP_Buzzer_Toggle(void);

#endif
