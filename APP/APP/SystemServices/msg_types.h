#ifndef MSG_TYPES_H
#define MSG_TYPES_H

// ---------- 消息类型枚举 ----------
typedef enum {
    MSG_NONE = 0,
    MSG_TICK_1S,
    MSG_TICK_200MS,
    MSG_KEY_SHORT,
    MSG_KEY_LONG,
    MSG_CMD_LED,
    MSG_CMD_OTA_START,
    MSG_CMD_SYSMON,
    // 可继续扩展...
    MSG_COUNT
} msg_type_t;

// ---------- 消息来源模块ID ----------
typedef enum {
    MODULE_NONE = 0,
    MODULE_TIMER,
    MODULE_KEY,
    MODULE_LED,
    MODULE_SHELL,
    MODULE_SYSMON,
    MODULE_OTA,
    MODULE_COUNT
} module_id_t;

#endif