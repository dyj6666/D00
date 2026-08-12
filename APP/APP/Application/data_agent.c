/* ================================================================
 * data_agent —— 数据代理：周期采集变量并上报上位机
 *
 * 架构位置：APP 应用层；独立 DataAgent 任务
 * 核心流程：按采样周期遍历注册变量 -> HOSTLINK 分帧上报
 * ================================================================ */
#include "data_agent.h"
#include "data_link.h"
#include "var_manager.h"
#include "app_config.h"
#include "protocol.h"
#include "var_ids.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "watchdog.h"
#include <string.h>

volatile int32_t g_system_tick = 0;     /* 系统 tick 快照，供上位机读取 */

static void DataAgentTaskFunc(void *arg)
{
    (void)arg;
    TaskHandle_t self = NULL;
    uint8_t wdg_registered = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HOSTLINK_SAMPLE_PERIOD_MS));

        if (!wdg_registered) {
            self = xTaskGetCurrentTaskHandle();
            WDOG_RegisterTask("DataAgent", self, 5000);
            wdg_registered = 1;
        }
        WDOG_Kick(self);    /* 心跳必须无条件上报，与是否有订阅无关 */

        g_system_tick = xTaskGetTickCount();

        uint16_t ids[HOSTLINK_MAX_SUBSCRIBE];
        uint8_t count;
        VAR_GetSubscribedList(ids, &count);
        if (count == 0) continue;

        /* 组包：帧头(5) + 订阅变量数据，超长时截断 */
        uint8_t buf[512];
        buf[0] = SYNC1;
        buf[1] = SYNC2;
        buf[2] = CMD_DATA;
        uint16_t idx = 5;                       /* 跳过 payload_len 字段 */

        for (uint8_t i = 0; i < count; i++) {
            uint16_t id = ids[i];
            uint16_t var_len = 0;
            uint8_t val_buf[8] = {0};
            if (VAR_Read(id, val_buf, &var_len) == 0) {
                /* 严格防止缓冲区溢出 */
                if ((uint16_t)(idx + 4 + var_len) > (uint16_t)(sizeof(buf) - 2)) break;
                buf[idx++] = id & 0xFF;
                buf[idx++] = (id >> 8) & 0xFF;
                buf[idx++] = var_len & 0xFF;
                buf[idx++] = 0;                 /* 保留 */
                memcpy(&buf[idx], val_buf, var_len);
                idx += var_len;
            }
        }

        uint16_t payload_len = idx - 5;
        buf[3] = payload_len & 0xFF;
        buf[4] = (payload_len >> 8) & 0xFF;

        DataLink_SendPacket(buf, idx);          /* 自动追加 CRC */
    }
}

void DataAgent_Init(void)
{
    /* 独立任务负责周期上报，避免占用定时器服务任务栈 */
    osThreadAttr_t attr = {
        .name = "DataAgent",
        .stack_size = 1024,   /* 峰值 ~664B（HW 218 词） */
        .priority = osPriorityNormal
    };
    osThreadNew(DataAgentTaskFunc, NULL, &attr);

    VAR_Register(VAR_ID_SYS_TICK, "sys_tick", VAR_TYPE_INT32, 0, (void *)&g_system_tick);
}
