/* ================================================================
 * ota_source —— 读源抽象实现
 * ================================================================ */
#include "ota_source.h"
#include "esp_flash.h"
#include "boot_config.h"

#include <string.h>

static uint32_t s_internal_base;   /* 内部源映射基址（OtaSource_Internal 设置） */

/* ---------------- 内部源：DOWNLOAD 内存直读 ---------------- */
static void internal_read(uint32_t off, void *buf, uint32_t len)
{
    memcpy(buf, (const void *)(s_internal_base + off), len);
}

void OtaSource_Internal(ota_source_t *s, uint32_t base_addr, uint32_t size)
{
    s_internal_base = base_addr;
    s->size = size;
    s->read = internal_read;
}

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
