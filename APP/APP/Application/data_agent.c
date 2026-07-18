#include "data_agent.h"
#include "data_link.h"
#include "var_manager.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include "logger.h"

static void DataAgentTaskFunc(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HOSTLINK_SAMPLE_PERIOD_MS));

        uint16_t ids[HOSTLINK_MAX_SUBSCRIBE];
        uint8_t count;
        VAR_GetSubscribedList(ids, &count);
        if (count == 0) continue;

        uint8_t buf[512];
        buf[0] = 0xAA;
        buf[1] = 0x55;
        buf[2] = 0x03;                           // CMD_DATA
        uint16_t idx = 5;                        // 跳过 payload_len (2字节)

        for (uint8_t i = 0; i < count; i++) {
            uint16_t id = ids[i];
            uint16_t var_len = 0;
            uint8_t val_buf[8] = {0};
            if (VAR_Read(id, val_buf, &var_len) == 0) {
                // 严格防止缓冲区溢出
                if (idx + 4 + var_len > sizeof(buf) - 2) break;
                buf[idx++] = id & 0xFF;
                buf[idx++] = (id >> 8) & 0xFF;
                buf[idx++] = var_len & 0xFF;
                buf[idx++] = 0;                   // 保留
                memcpy(&buf[idx], val_buf, var_len);
                idx += var_len;
            }
        }

        uint16_t payload_len = idx - 5;
        buf[3] = payload_len & 0xFF;
        buf[4] = (payload_len >> 8) & 0xFF;

        DataLink_SendPacket(buf, idx);             // 自动追加 CRC
    }
}

void DataAgent_Init(void)
{
    // 创建独立任务，栈 1024 字，优先级 Normal
    osThreadAttr_t attr = {
        .name = "DataAgent",
        .stack_size = 2048,
        .priority = osPriorityNormal
    };
    osThreadNew(DataAgentTaskFunc, NULL, &attr);
}