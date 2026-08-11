/* ================================================================
 * la_trigger —— 逻辑分析仪触发：边沿/电平状态机
 *
 * 架构位置：APP 服务层；采样启动门控
 * ================================================================ */
#include "la_trigger.h"

#include <stddef.h>

static la_trigger_cfg_t trigger_cfg = {
    .type = LA_TRIG_NONE,
    .channel = 0,
    .post_samples = 2048,
    .cond_channel = 0xFF,
    .cond_level = 1,
};
static uint8_t triggered = 0;
static uint8_t armed = 0;

void LA_Trigger_Init(void)
{
    trigger_cfg.type = LA_TRIG_NONE;
    trigger_cfg.channel = 0;
    trigger_cfg.post_samples = 2048;
    trigger_cfg.cond_channel = 0xFF;
    trigger_cfg.cond_level = 1;
    triggered = 0;
    armed = 0;
}

void LA_Trigger_Set(LA_TriggerType type, uint8_t channel)
{
    trigger_cfg.type = type;
    trigger_cfg.channel = channel;
    trigger_cfg.cond_channel = 0xFF;   /* 无条件 */
    LA_Trigger_Reset();
}

void LA_Trigger_SetConfig(const la_trigger_cfg_t *cfg)
{
    if (cfg == NULL) return;
    trigger_cfg = *cfg;
    if (trigger_cfg.channel >= LA_MAX_CHANNELS) trigger_cfg.channel = 0;
    if (trigger_cfg.post_samples == 0) trigger_cfg.post_samples = 2048;
    if (trigger_cfg.cond_channel >= LA_MAX_CHANNELS) trigger_cfg.cond_channel = 0xFF;
    trigger_cfg.cond_level = (trigger_cfg.cond_level != 0);
    LA_Trigger_Reset();
}

void LA_Trigger_GetConfig(la_trigger_cfg_t *out)
{
    if (out != NULL) *out = trigger_cfg;
}

void LA_Trigger_Arm(void)
{
    armed = 1;
    triggered = 0;
}

uint8_t LA_Trigger_IsTriggered(void)
{
    return triggered;
}

uint8_t LA_Trigger_IsArmed(void)
{
    return armed;
}

void LA_Trigger_Reset(void)
{
    armed = 0;
    triggered = 0;
}

LA_TriggerType LA_Trigger_GetType(void)
{
    return trigger_cfg.type;
}

uint8_t LA_Trigger_GetChannel(void)
{
    return trigger_cfg.channel;
}

void LA_Trigger_SetTriggered(void)
{
    triggered = 1;
    armed = 0;                          /* 触发一次后解除武装，避免重复触发 */
}
