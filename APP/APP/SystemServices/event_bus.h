#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "FreeRTOS.h"
#include "msg_types.h"
#include "queue.h"

#include <stdint.h>
#include <string.h>

/* ---------- 强类型消息体 ---------- */
typedef struct {
    uint16_t src;      // 发送者模块ID
    uint16_t type;     // 消息类型
} msg_hdr_t;

typedef struct {
    msg_hdr_t hdr;
    uint16_t len;           // payload 长度
    uint8_t  payload[];     // 柔性数组
} message_t;

/* ---------- 回调签名 ---------- */
typedef void (*msg_handler_t)(const message_t *msg);

/* ---------- 总线接口 ---------- */
void EventBus_Init(void);

/* 从静态池分配一条消息（任务上下文）。成功返回 0，*out 指向已初始化消息。
 * 失败返回 -1（池空）或 -3（payload 超长）。 */
int  EventBus_AllocMsg(uint16_t src, uint16_t type, uint16_t len, message_t **out);

/* 归还消息到静态池（任务/中断上下文均安全）。 */
void EventBus_FreeMsg(message_t *msg);

/* 发布消息。无论成功失败，调用后 msg 均不再归调用方所有
 * （成功由总线分发后回收，失败立即回收）。返回 0 成功。 */
int  EventBus_Publish(message_t *msg);       // 任务上下文
int  EventBus_PublishFromISR(message_t *msg);// 中断上下文

int  EventBus_Subscribe(uint16_t type, msg_handler_t handler);
void EventBusTaskFunction(void);
uint32_t EventBus_GetLostCount(void);
uint32_t EventBus_GetPoolFreeCount(void);
uint32_t EventBus_GetQueueCount(void);

/* ---------- 便捷发布宏 ---------- */
#define MSG_SEND_SIMPLE(src_id, msg_type) do { \
    message_t *msg = NULL; \
    if (EventBus_AllocMsg((src_id), (msg_type), 0, &msg) == 0) { \
        EventBus_Publish(msg); /* 失败时内部已回收 */ \
    } \
} while(0)

#define MSG_SEND_DATA(src_id, msg_type, pdata, dlen) do { \
    message_t *msg = NULL; \
    if (EventBus_AllocMsg((src_id), (msg_type), (dlen), &msg) == 0) { \
        memcpy(msg->payload, (pdata), (dlen)); \
        EventBus_Publish(msg); /* 失败时内部已回收 */ \
    } \
} while(0)

#endif
