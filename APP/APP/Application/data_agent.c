#include "data_agent.h"
#include "data_link.h"
#include "var_manager.h"
#include "app_config.h"
#include "cmsis_os.h"
#include "timers.h"
#include <string.h>

static TimerHandle_t sample_timer;

static void sample_callback(TimerHandle_t xTimer) {
    uint16_t ids[HOSTLINK_MAX_SUBSCRIBE];
    uint8_t count;
    VAR_GetSubscribedList(ids, &count);
    if (count == 0) return;

    // 组装数据包
    uint8_t buf[512];
    buf[0] = 0xAA; buf[1] = 0x55;
    buf[2] = 0x03; // CMD_DATA
    uint16_t idx = 5; // 跳过 len(2B)
    for (uint8_t i = 0; i < count; i++) {
        uint16_t id = ids[i];
        uint16_t var_len;
        uint8_t val_buf[8];
        if (VAR_Read(id, val_buf, &var_len) == 0) {
            if (idx + 4 + var_len + 1 > sizeof(buf)) break; // 防止溢出
            buf[idx++] = id & 0xFF;
            buf[idx++] = (id >> 8) & 0xFF;
            buf[idx++] = var_len & 0xFF;
            buf[idx++] = 0; // 保留
            memcpy(&buf[idx], val_buf, var_len);
            idx += var_len;
        }
    }

    uint16_t payload_len = idx - 5;
    buf[3] = payload_len & 0xFF;
    buf[4] = (payload_len >> 8) & 0xFF;
    uint8_t crc = 0;
    for (uint16_t i = 0; i < idx; i++) crc ^= buf[i];
    buf[idx++] = crc;

    DataLink_SendPacket(buf, idx);
}

void DataAgent_Init(void) {
    sample_timer = xTimerCreate("dagent", pdMS_TO_TICKS(HOSTLINK_SAMPLE_PERIOD_MS),
                                pdTRUE, NULL, sample_callback);
    if (sample_timer) {
        xTimerStart(sample_timer, 0);
    }
}