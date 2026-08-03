#include "ota_agent.h"
#include "event_bus.h"
#include "logger.h"
#include "app_config.h"
#include "bsp.h"

static void handle_ota_msg(const message_t *msg)
{
    if (msg == NULL) return;
    if (msg->hdr.type != MSG_CMD_OTA_START) return;

    LOG_Printf("APP: Received upgrade command. Entering BOOT...\r\n");

    /* 写入升级标志到备份寄存器，然后软复位进入 BOOT */
    BSP_RTC_WriteBackupReg(0, BOOT_FLAG_UPGRADE);
    BSP_DelayMs(100);           /* 确保写入完成 */
    BSP_SystemReset();
}

void OtaAgent_Init(void)
{
    EventBus_Subscribe(MSG_CMD_OTA_START, handle_ota_msg);
    LOG_Printf("OTA Agent initialized.\r\n");
}
