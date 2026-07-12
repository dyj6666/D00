#include "event_bus.h"
#include <string.h>
#include "logger.h"

#define SUBSCRIBERS_MAX   EVENT_BUS_SUBS_MAX

static int bus_initialized = 0;

typedef struct {
    event_handler_t handlers[SUBSCRIBERS_MAX];
    uint8_t count;
} subs_list_t;

static subs_list_t subs[EVENT_COUNT];
static QueueHandle_t evt_queue;

void EventBus_Init(void) {
    if (bus_initialized) return;
    memset(subs, 0, sizeof(subs));
    evt_queue = xQueueCreate(EVENT_BUS_QUEUE_LENGTH, sizeof(event_packet_t));
    if (evt_queue == NULL) {
        while(1) {}
    }
    bus_initialized = 1;
}

int EventBus_Subscribe(event_id_t evt, event_handler_t handler) {
    if (evt >= EVENT_COUNT || handler == NULL) return -1;
    subs_list_t *list = &subs[evt];
    if (list->count >= SUBSCRIBERS_MAX) return -2;
    list->handlers[list->count++] = handler;
    return 0;
}

void EventBus_Publish(event_id_t evt, const void *payload, uint32_t len) {
    event_packet_t pkt = {evt, (void*)payload, len};
    xQueueSend(evt_queue, &pkt, 0);
}

void EventBus_PublishFromISR(event_id_t evt, const void *payload, uint32_t len) {
    event_packet_t pkt = {evt, (void*)payload, len};
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(evt_queue, &pkt, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void dispatch_event(const event_packet_t *pkt) {
    if (pkt->id >= EVENT_COUNT) return;
    subs_list_t *list = &subs[pkt->id];
    for (uint8_t i = 0; i < list->count; i++) {
        list->handlers[i](pkt->id, pkt->payload, pkt->len);
    }
}

void EventBusTaskFunction(void) {
    event_packet_t pkt;
    for (;;) {
        if (xQueueReceive(evt_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            dispatch_event(&pkt);
        }
    }
}
