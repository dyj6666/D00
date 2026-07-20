#include "event_bus.h"
#include "app_config.h"
#include "logger.h"

#define SUBSCRIBERS_MAX  4

typedef struct {
    msg_handler_t handlers[SUBSCRIBERS_MAX];
    uint8_t count;
} subs_list_t;

static subs_list_t subs[MSG_COUNT];
static QueueHandle_t msg_queue;  // 存储 message_t*

void EventBus_Init(void) {
    memset(subs, 0, sizeof(subs));
    msg_queue = xQueueCreate(EVENT_BUS_QUEUE_LENGTH, sizeof(message_t*));
    if (msg_queue == NULL) {
        while(1) {}  // 致命错误
    }
}

int EventBus_Subscribe(uint16_t type, msg_handler_t handler) {
    if (type >= MSG_COUNT || handler == NULL) return -1;
    subs_list_t *list = &subs[type];
    if (list->count >= SUBSCRIBERS_MAX) return -2;
    list->handlers[list->count++] = handler;
    return 0;
}

void EventBus_Publish(message_t *msg) {
    xQueueSend(msg_queue, &msg, 0);  // 非阻塞
}

void EventBus_PublishFromISR(message_t *msg) {
    BaseType_t xHPTW = pdFALSE;
    xQueueSendFromISR(msg_queue, &msg, &xHPTW);
    portYIELD_FROM_ISR(xHPTW);
}

static void dispatch_message(const message_t *msg) {
    if (msg->hdr.type >= MSG_COUNT) return;
    subs_list_t *list = &subs[msg->hdr.type];
    for (uint8_t i = 0; i < list->count; i++) {
        list->handlers[i](msg);
    }
    vPortFree((void*)msg);  // 分发完毕释放内存
}

void EventBusTaskFunction(void) {
    message_t *msg;
    for (;;) {
        if (xQueueReceive(msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            dispatch_message(msg);
        }
    }
}