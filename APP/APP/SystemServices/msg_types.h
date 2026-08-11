/* ================================================================
 * msg_types —— 事件总线消息类型定义
 *
 * 架构位置：APP 配置层；模块间通信契约
 * ================================================================ */
/* ================================================================
 * msg_types —— 事件总线消息类型定义
 *
 * 架构位置：APP 配置层；模块间通信契约
 * ================================================================ */
#ifndef MSG_TYPES_H
#define MSG_TYPES_H

/* ---------- 消息类型枚举 ---------- */
typedef enum {
    MSG_NONE = 0,
    MSG_TICK_1S,
    MSG_TICK_200MS,
    MSG_KEY_SHORT,
    MSG_KEY_LONG,
    MSG_CMD_LED,
    MSG_CMD_OTA_START,
    MSG_CMD_SYSMON,
    MSG_EB_STRESS,
    /* 可继续扩展... */
    MSG_COUNT
} msg_type_t;

/* ---------- 消息来源模块 ID ---------- */
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
