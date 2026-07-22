#ifndef EVENT_BUS_H
#define EVENT_BUS_H
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "msg_types.h"

/*---------------- 强类型消息体 -----------------*/
typedef struct {
    uint16_t src;      // 发送者模块ID
    uint16_t type;     // 消息类型
} msg_hdr_t;

typedef struct {
    msg_hdr_t hdr;
    uint16_t len;           // payload 长度
    uint8_t  payload[];     // 柔性数组
} message_t;

/*---------------- 回调签名 -----------------*/
typedef void (*msg_handler_t)(const message_t *msg);

/*---------------- 总线接口 -----------------*/
void EventBus_Init(void);
int  EventBus_Publish(message_t *msg);       // 任务上下文，返回0成功
int  EventBus_PublishFromISR(message_t *msg);// 中断上下文，返回0成功
int  EventBus_Subscribe(uint16_t type, msg_handler_t handler);
void EventBusTaskFunction(void);
uint32_t EventBus_GetLostCount(void);

/*---------------- 便捷发布宏 -----------------*/
#define MSG_SEND_SIMPLE(src_id, msg_type) do { \
    message_t *msg = pvPortMalloc(sizeof(message_t)); \
    if (msg) { \
        msg->hdr.src = (src_id); \
        msg->hdr.type = (msg_type); \
        msg->len = 0; \
        if (EventBus_Publish(msg) != 0) { \
            /* 发布失败，内存已由Publish内部释放 */ \
        } \
    } \
} while(0)

#define MSG_SEND_DATA(src_id, msg_type, pdata, dlen) do { \
    message_t *msg = pvPortMalloc(sizeof(message_t) + (dlen)); \
    if (msg) { \
        msg->hdr.src = (src_id); \
        msg->hdr.type = (msg_type); \
        msg->len = (dlen); \
        memcpy(msg->payload, (pdata), (dlen)); \
        if (EventBus_Publish(msg) != 0) { \
            /* 发布失败，内存已由Publish内部释放 */ \
        } \
    } \
} while(0)

#endif