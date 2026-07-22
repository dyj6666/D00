#include "event_bus.h"
#include "app_config.h"
#include "logger.h"
#include <string.h>

#define SUBSCRIBERS_MAX  EVENT_BUS_SUBS_MAX

typedef struct {
    msg_handler_t handlers[SUBSCRIBERS_MAX];
    uint8_t count;
} subs_list_t;

static subs_list_t subs[MSG_COUNT];
static QueueHandle_t msg_queue;           // 主事件队列
static QueueHandle_t dead_letter_queue;   // 释放队列（用于ISR中安全释放内存）
static volatile uint32_t g_msg_lost_count = 0; // 消息丢失计数器

void EventBus_Init(void) {
    memset(subs, 0, sizeof(subs));
    msg_queue = xQueueCreate(EVENT_BUS_QUEUE_LENGTH, sizeof(message_t*));
    dead_letter_queue = xQueueCreate(EVENT_BUS_DEAD_LETTER_LEN, sizeof(message_t*));
    if (msg_queue == NULL || dead_letter_queue == NULL) {
        // 致命错误，无法恢复
        while(1) {}
    }
    g_msg_lost_count = 0;
}

int EventBus_Subscribe(uint16_t type, msg_handler_t handler) {
    if (type >= MSG_COUNT || handler == NULL) return -1;
    subs_list_t *list = &subs[type];
    if (list->count >= SUBSCRIBERS_MAX) return -2;
    list->handlers[list->count++] = handler;
    return 0;
}

int EventBus_Publish(message_t *msg) {
    if (msg == NULL) return -1;
    if (xQueueSend(msg_queue, &msg, 0) != pdTRUE) {
        // 队列满，立即释放内存并记录丢失
        g_msg_lost_count++;
        vPortFree(msg);
        return -1;
    }
    return 0;
}

int EventBus_PublishFromISR(message_t *msg) {
    if (msg == NULL) return -1;
    BaseType_t xHPTW = pdFALSE;
    if (xQueueSendFromISR(msg_queue, &msg, &xHPTW) != pdTRUE) {
        // 主队列满，放入释放队列，等待任务上下文释放
        if (xQueueSendFromISR(dead_letter_queue, &msg, &xHPTW) != pdTRUE) {
            // 连释放队列也满了，只能放弃并记录
            g_msg_lost_count++;
        }
        portYIELD_FROM_ISR(xHPTW);
        return -1;
    }
    portYIELD_FROM_ISR(xHPTW);
    return 0;
}

static void dispatch_message(message_t *msg) {
    if (msg == NULL) return;
    uint16_t type = msg->hdr.type;
    if (type >= MSG_COUNT) {
        vPortFree(msg);
        return;
    }
    subs_list_t *list = &subs[type];
    for (uint8_t i = 0; i < list->count; i++) {
        list->handlers[i](msg);
    }
    vPortFree(msg);  // 处理完毕，释放内存
}

void EventBusTaskFunction(void) {
    message_t *msg;
    for (;;) {
        // 优先处理主队列中的消息
        if (xQueueReceive(msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            dispatch_message(msg);
        }
        // 处理释放队列中的消息（ISR中发布失败时放入的）
        while (xQueueReceive(dead_letter_queue, &msg, 0) == pdTRUE) {
            vPortFree(msg); // 安全地在任务上下文中释放内存
        }
    }
}

uint32_t EventBus_GetLostCount(void) {
    return g_msg_lost_count;
}