#ifndef EVENT_BUS_H
#define EVENT_BUS_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "app_config.h"

typedef enum {
    EVENT_NONE = 0,
    EVENT_TICK_1S,
    EVENT_TICK_200MS,
    EVENT_KEY_SHORT,
    EVENT_KEY_LONG,
    EVENT_CMD_LED,          // Shell 命令映射事件
    EVENT_CMD_OTA_START,
    EVENT_COUNT
} event_id_t;

typedef void (*event_handler_t)(event_id_t evt, const void *payload, uint32_t len);

typedef struct {
    event_id_t  id;
    void       *payload;
    uint32_t    len;
} event_packet_t;

void EventBus_Init(void);
void EventBus_Publish(event_id_t evt, const void *payload, uint32_t len);
void EventBus_PublishFromISR(event_id_t evt, const void *payload, uint32_t len);
int  EventBus_Subscribe(event_id_t evt, event_handler_t handler);

void EventBusTaskFunction(void);

#endif

