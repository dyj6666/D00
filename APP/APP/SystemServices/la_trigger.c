#include "la_trigger.h"

static LA_TriggerType trigger_type = LA_TRIG_NONE;
static uint8_t trigger_channel = 0;
static uint8_t triggered = 0;
static uint8_t armed = 0;

void LA_Trigger_Init(void)
{
    trigger_type = LA_TRIG_NONE;    /* 默认为无触发，即连续采集 */
    trigger_channel = 0;
    triggered = 0;
    armed = 0;
}

void LA_Trigger_Set(LA_TriggerType type, uint8_t channel)
{
    trigger_type = type;
    trigger_channel = channel;
    LA_Trigger_Reset();
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
    return trigger_type;
}

uint8_t LA_Trigger_GetChannel(void)
{
    return trigger_channel;
}

void LA_Trigger_SetTriggered(void)
{
    triggered = 1;
    armed = 0;                      /* 触发一次后解除武装，避免重复触发 */
}
