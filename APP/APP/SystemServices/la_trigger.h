#ifndef LA_TRIGGER_H
#define LA_TRIGGER_H

#include "la_config.h"

void LA_Trigger_Init(void);

/* 无条件边沿触发（兼容接口：cond_channel = 0xFF） */
void LA_Trigger_Set(LA_TriggerType type, uint8_t channel);

/* 完整触发配置（支持条件触发：边沿 + 条件通道电平） */
void LA_Trigger_SetConfig(const la_trigger_cfg_t *cfg);
void LA_Trigger_GetConfig(la_trigger_cfg_t *out);

void LA_Trigger_Arm(void);
uint8_t LA_Trigger_IsTriggered(void);
uint8_t LA_Trigger_IsArmed(void);
void LA_Trigger_Reset(void);

/* 便捷读取 */
LA_TriggerType LA_Trigger_GetType(void);
uint8_t LA_Trigger_GetChannel(void);

void LA_Trigger_SetTriggered(void);

#endif
