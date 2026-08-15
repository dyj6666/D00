/* ================================================================
 * ota_source —— 读源抽象实现（方案B：外部 SPI Flash 单一源）
 * ================================================================ */
#include "ota_source.h"
#include "esp_flash.h"
#include "boot_config.h"

/* ---------------- 外部源：SPI Flash 读 ---------------- */
static void external_read(uint32_t off, void *buf, uint32_t len)
{
    (void)EspFlash_Read(ESP_OTA_BASE + off, buf, len);
}

bool OtaSource_External(ota_source_t *s)
{
    uint32_t total = 0u;
    if (!EspFlash_HasPackage(&total)) {
        return false;
    }
    s->size = total;
    s->read = external_read;
    return true;
}
