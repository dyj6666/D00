#include "event_bus.h"
#include "app_config.h"
#include "watchdog.h"

#include <string.h>

#define SUBSCRIBERS_MAX  EVENT_BUS_SUBS_MAX

typedef struct {
    msg_handler_t handlers[SUBSCRIBERS_MAX];
    uint8_t count;
} subs_list_t;

static subs_list_t subs[MSG_COUNT];
static QueueHandle_t msg_queue;           // 主事件队列（存消息指针）

/* ---------------- 静态消息池 ---------------- */
/* 注意：AC5 不允许含柔性数组成员（payload[]）的类型作为数组元素，
 * 因此槽位用同布局的定长结构；使用时按 message_t* 访问。 */
typedef struct {
    msg_hdr_t hdr;
    uint16_t  len;
    uint8_t   payload[EVENT_BUS_MSG_MAX_PAYLOAD];
} msg_slot_t;

/* 消息槽池：纯 CPU 访问，放 CCM（主 SRAM 让给 DMA/ETH） */
static msg_slot_t g_msg_pool[EVENT_BUS_POOL_SIZE]
    __attribute__((section(".ccmram"), zero_init));
static QueueHandle_t free_queue;          // 空闲槽队列（ISR 安全）

static volatile uint32_t g_msg_lost_count = 0; // 消息丢失计数器

void EventBus_Init(void)
{
    memset(subs, 0, sizeof(subs));
    msg_queue = xQueueCreate(EVENT_BUS_QUEUE_LENGTH, sizeof(message_t*));
    free_queue = xQueueCreate(EVENT_BUS_POOL_SIZE, sizeof(message_t*));
    if (msg_queue == NULL || free_queue == NULL) {
        while (1) {}   // 致命错误，无法恢复
    }

    /* 把全部槽位放入空闲池 */
    for (uint32_t i = 0; i < EVENT_BUS_POOL_SIZE; i++) {
        message_t *slot = (message_t *)&g_msg_pool[i];
        xQueueSend(free_queue, &slot, 0);
    }
    g_msg_lost_count = 0;
}

int EventBus_AllocMsg(uint16_t src, uint16_t type, uint16_t len, message_t **out)
{
    message_t *slot = NULL;

    if (out == NULL) return -1;
    if (len > EVENT_BUS_MSG_MAX_PAYLOAD) return -3;
    if (xQueueReceive(free_queue, &slot, 0) != pdTRUE) {
        g_msg_lost_count++;
        return -1;   /* 池空 */
    }

    slot->hdr.src = src;
    slot->hdr.type = type;
    slot->len = len;
    *out = slot;
    return 0;
}

void EventBus_FreeMsg(message_t *msg)
{
    if (msg == NULL) return;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    /* FromISR 变体在任务上下文同样可用；池只进不出，不会满 */
    (void)xQueueSendFromISR(free_queue, &msg, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

int EventBus_Publish(message_t *msg)
{
    if (msg == NULL) return -1;
    if (xQueueSend(msg_queue, &msg, 0) != pdTRUE) {
        /* 队列满：立即回收，记录丢失 */
        g_msg_lost_count++;
        EventBus_FreeMsg(msg);
        return -1;
    }
    return 0;
}

int EventBus_PublishFromISR(message_t *msg)
{
    if (msg == NULL) return -1;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(msg_queue, &msg, &xHigherPriorityTaskWoken) != pdTRUE) {
        g_msg_lost_count++;
        EventBus_FreeMsg(msg);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return -1;
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return 0;
}

int EventBus_Subscribe(uint16_t type, msg_handler_t handler)
{
    if (type >= MSG_COUNT || handler == NULL) return -1;
    subs_list_t *list = &subs[type];
    if (list->count >= SUBSCRIBERS_MAX) return -2;
    list->handlers[list->count++] = handler;
    return 0;
}

static void dispatch_message(message_t *msg)
{
    if (msg == NULL) return;
    uint16_t type = msg->hdr.type;
    if (type >= MSG_COUNT) {
        EventBus_FreeMsg(msg);
        return;
    }
    subs_list_t *list = &subs[type];
    for (uint8_t i = 0; i < list->count; i++) {
        list->handlers[i](msg);
    }
    EventBus_FreeMsg(msg);   /* 处理完毕，归还池 */
}

void EventBusTaskFunction(void)
{
    message_t *msg;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    WDOG_RegisterTask("EventBus", self, 5000);
    for (;;) {
        /* 带超时接收：总线空闲时也周期性上报心跳，避免静默误判 */
        if (xQueueReceive(msg_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            dispatch_message(msg);
        }
        WDOG_Kick(self);
    }
}

uint32_t EventBus_GetLostCount(void)
{
    return g_msg_lost_count;
}

uint32_t EventBus_GetPoolFreeCount(void)
{
    return (uint32_t)uxQueueMessagesWaiting(free_queue);
}

uint32_t EventBus_GetQueueCount(void)
{
    return (uint32_t)uxQueueMessagesWaiting(msg_queue);
}
