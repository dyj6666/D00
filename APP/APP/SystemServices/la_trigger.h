#ifndef LA_TRIGGER_H
#define LA_TRIGGER_H

#include "la_config.h"

void LA_Trigger_Init(void);
void LA_Trigger_Set(LA_TriggerType type, uint8_t channel);
void LA_Trigger_Arm(void);
uint8_t LA_Trigger_IsTriggered(void);
uint8_t LA_Trigger_IsArmed(void);
void LA_Trigger_Reset(void);

/* 获取当前触发配置（供采样引擎使用） */
LA_TriggerType LA_Trigger_GetType(void);
uint8_t LA_Trigger_GetChannel(void);
void LA_Trigger_SetTriggered(void);

#endif
