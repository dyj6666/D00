#include "ota_agent.h"
#include "event_bus.h"
#include "logger.h"
#include "pinout.h"
#include "main.h"        // 提供 HAL 句柄，如 hrtc
#include "app_config.h"  // 可能需要 BOOT_FLAG_UPGRADE 等宏

extern RTC_HandleTypeDef hrtc;
static void handle_ota_cmd(event_id_t evt, const void *payload, uint32_t len)
{
    (void)payload;
    (void)len;
    if (evt != EVENT_CMD_OTA_START) return;

    LOG_Printf("APP: Received upgrade command. Entering BOOT...\r\n");

    // 启用备份域访问
    HAL_PWR_EnableBkUpAccess();
    // 写入升级标志到备份寄存器
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, BOOT_FLAG_UPGRADE);
    // 延时确保写入完成
    HAL_Delay(100);
    // 软件复位
    NVIC_SystemReset();
}

void OtaAgent_Init(void)
{
    EventBus_Subscribe(EVENT_CMD_OTA_START, handle_ota_cmd);
    LOG_Printf("OTA Agent initialized.\r\n");
}