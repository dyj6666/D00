/* ================================================================
 * 多协议 OTA 传输注册表实现
 * ================================================================ */
#include "ota_transport.h"
#include "logger.h"

#include <string.h>

#define OTA_TRANSPORT_MAX  6

static ota_transport_t s_table[OTA_TRANSPORT_MAX];
static uint8_t s_count = 0;

int OtaMgr_Register(const ota_transport_t *t)
{
    if (t == NULL || s_count >= OTA_TRANSPORT_MAX) {
        return -1;
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_table[i].id == t->id) {
            return -2;
        }
    }
    s_table[s_count++] = *t;
    LOG_Printf("[OTA] transport %s registered (%s)\r\n",
               t->name, t->available ? "ready" : "reserved");
    return 0;
}

uint8_t OtaMgr_Count(void)
{
    return s_count;
}

const ota_transport_t *OtaMgr_Get(uint8_t i)
{
    if (i >= s_count) {
        return NULL;
    }
    return &s_table[i];
}

void OtaMgr_Init(void)
{
    s_count = 0;
    memset(s_table, 0, sizeof(s_table));

    static const ota_transport_t uart = {
        OTA_TRANSPORT_UART, "UART", "HOSTLINK serial (COM13)", 1,
    };
    static const ota_transport_t tcp = {
        OTA_TRANSPORT_ETH_TCP, "ETH-TCP", "TCP server :9020", 1,
    };
    static const ota_transport_t http = {
        OTA_TRANSPORT_ETH_HTTP, "ETH-HTTP", "HTTP client pull", 1,
    };
    static const ota_transport_t can = {
        OTA_TRANSPORT_CAN, "CAN", "CAN bus (reserved)", 0,
    };
    (void)OtaMgr_Register(&uart);
    (void)OtaMgr_Register(&tcp);
    (void)OtaMgr_Register(&http);
    (void)OtaMgr_Register(&can);
}
