/* OTA Agent：运行时固件下载到 DOWNLOAD 区 + 启动确认（A/B 回滚体系配套）。
 *
 * 流程：上位机 HOSTLINK 发 BEGIN/DATA/END -> 固件写入 DOWNLOAD 区 ->
 *       置备份域升级标志并复位 -> BOOT 备份当前 RUN、切换新固件、
 *       新固件启动后由本模块向参数区写"确认成功"（未确认则 BOOT 回滚）。
 * 安全（签名/解密/版本防回滚）由 BOOT 完成，本模块只保证传输完整。
 */
#include "ota_agent.h"

#include <string.h>
#include <stdbool.h>

#include "app_config.h"
#include "bsp.h"
#include "event_bus.h"
#include "logger.h"
#include "msg_types.h"
#include "stm32f4xx_hal.h"

/* ---------------- 参数区（与 BOOT/boot_param.c 结构一致） ---------------- */
#pragma pack(1)
typedef struct {
    uint32_t magic;
    uint32_t boot_state;
    uint32_t boot_count;
    uint32_t rollback_count;
    uint32_t last_error;
    uint32_t crc32;
} ota_param_t;
#pragma pack()

/* ---------------- 内部状态 ---------------- */
static volatile uint8_t  ota_state = OTA_ST_IDLE;
static uint32_t ota_total = 0;        /* 固件总大小 */
static uint32_t ota_received = 0;     /* 已收字节 */

/* shell "ota" 命令：写 BKP 升级标志并复位，触发 BOOT 升级模式 */
static void handle_ota_start_msg(const message_t *msg)
{
    if (msg == NULL || msg->hdr.type != MSG_CMD_OTA_START) {
        return;
    }
    LOG_Printf("OTA: entering BOOT upgrade mode...\r\n");
    BSP_RTC_WriteBackupReg(0, BOOT_FLAG_UPGRADE);
    BSP_DelayMs(100);
    BSP_SystemReset();
}

/* ---------------- Flash 操作（HAL） ---------------- */
static bool ota_flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t sector = 0;
    if (addr >= 0x08000000 && addr < 0x08004000) sector = FLASH_SECTOR_0;
    else if (addr < 0x08008000) sector = FLASH_SECTOR_1;
    else if (addr < 0x0800C000) sector = FLASH_SECTOR_2;
    else if (addr < 0x08010000) sector = FLASH_SECTOR_3;
    else if (addr < 0x08020000) sector = FLASH_SECTOR_4;
    else if (addr < 0x08040000) sector = FLASH_SECTOR_5;
    else if (addr < 0x08060000) sector = FLASH_SECTOR_6;
    else if (addr < 0x08080000) sector = FLASH_SECTOR_7;
    else if (addr < 0x080A0000) sector = FLASH_SECTOR_8;
    else if (addr < 0x080C0000) sector = FLASH_SECTOR_9;
    else if (addr < 0x080E0000) sector = FLASH_SECTOR_10;
    else sector = FLASH_SECTOR_11;

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef er = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .Sector = sector,
        .NbSectors = 1,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t err = 0;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &err);
    HAL_FLASH_Lock();
    (void)len;
    return (st == HAL_OK && err == 0xFFFFFFFF);
}

static bool ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) return true;
    HAL_FLASH_Unlock();

    /* 前导非对齐字节 */
    while ((addr & 0x03) && len > 0) {
        uint32_t wa = addr & ~0x03u;
        uint32_t val = *(volatile uint32_t *)wa;
        ((uint8_t *)&val)[addr & 0x03] = *data;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, wa, val) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        addr++; data++; len--;
    }
    while (len >= 4) {
        uint32_t word;
        memcpy(&word, data, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        addr += 4; data += 4; len -= 4;
    }
    /* 尾部非对齐 */
    while (len > 0) {
        uint32_t wa = addr & ~0x03u;
        uint32_t val = *(volatile uint32_t *)wa;
        ((uint8_t *)&val)[addr & 0x03] = *data;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, wa, val) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        addr++; data++; len--;
    }
    HAL_FLASH_Lock();
    return true;
}

static uint32_t ota_param_crc(const ota_param_t *p)
{
    const uint8_t *d = (const uint8_t *)p;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sizeof(ota_param_t); i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

/* 启动确认：BOOT 置 PENDING 后，新固件首次正常运行即写 NORMAL */
static void ota_confirm_startup(void)
{
    ota_param_t p;
    memcpy(&p, (const void *)OTA_PARAM_ADDR, sizeof(p));
    if (p.magic != OTA_PARAM_MAGIC || p.crc32 != ota_param_crc(&p)) {
        return;   /* 参数无效：交由 BOOT 处理 */
    }
    if (p.boot_state != OTA_STATE_PENDING) {
        return;
    }
    LOG_Printf("OTA: new firmware confirmed (tries=%lu)\r\n",
               (unsigned long)p.boot_count);
    p.boot_state = OTA_STATE_NORMAL;
    p.boot_count = 0;
    p.crc32 = ota_param_crc(&p);
    if (ota_flash_erase(OTA_PARAM_ADDR, 0) &&
        ota_flash_write(OTA_PARAM_ADDR, (const uint8_t *)&p, sizeof(p)) &&
        ota_flash_write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                        (const uint8_t *)&p, sizeof(p))) {
        LOG_Printf("OTA: startup confirmed OK\r\n");
    } else {
        LOG_Printf("OTA: confirm write FAILED\r\n");
    }
}

/* ---------------- HOSTLINK 命令入口 ---------------- */
uint8_t Ota_Begin(uint32_t version, uint32_t size)
{
    if (ota_state == OTA_ST_RECEIVING) {
        return 1;   /* 已在接收中 */
    }
    if (size == 0 || size > OTA_DOWNLOAD_SAFE) {
        LOG_Printf("OTA: bad size %lu\r\n", (unsigned long)size);
        return 2;
    }
    /* 版本降级拦截（BOOT 侧还会二次校验） */
    uint32_t cur = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    if (cur != 0xFFFFFFFFu && cur != 0u && version < cur) {
        LOG_Printf("OTA: version downgrade denied (%lu < %lu)\r\n",
                   (unsigned long)version, (unsigned long)cur);
        return 4;
    }
    LOG_Printf("OTA: begin v%lu size=%lu, erasing download area...\r\n",
               (unsigned long)version, (unsigned long)size);
    if (!ota_flash_erase(OTA_DOWNLOAD_ADDR, 0)) {
        LOG_Printf("OTA: download erase FAILED\r\n");
        return 3;
    }
    ota_total = size;
    ota_received = 0;
    ota_state = OTA_ST_RECEIVING;
    LOG_Printf("OTA: download area ready, awaiting data\r\n");
    return 0;
}

uint8_t Ota_Data(uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (ota_state != OTA_ST_RECEIVING) {
        return 1;
    }
    if (len > OTA_CHUNK_MAX || offset != ota_received ||
        offset + len > ota_total || offset + len > OTA_DOWNLOAD_SAFE) {
        LOG_Printf("OTA: bad chunk off=%lu len=%u\r\n",
                   (unsigned long)offset, (unsigned)len);
        return 2;
    }
    if (!ota_flash_write(OTA_DOWNLOAD_ADDR + offset, data, len)) {
        LOG_Printf("OTA: flash write FAILED at %lu\r\n",
                   (unsigned long)offset);
        ota_state = OTA_ST_IDLE;
        return 3;
    }
    ota_received += len;
    return 0;
}

uint8_t Ota_End(void)
{
    if (ota_state != OTA_ST_RECEIVING) {
        return 1;
    }
    if (ota_received != ota_total) {
        LOG_Printf("OTA: incomplete %lu/%lu\r\n",
                   (unsigned long)ota_received, (unsigned long)ota_total);
        ota_state = OTA_ST_IDLE;
        return 2;
    }
    ota_state = OTA_ST_DONE;
    LOG_Printf("OTA: download complete (%lu B), rebooting to BOOT...\r\n",
               (unsigned long)ota_total);

    /* 触发 BOOT 升级模式（双保险）：
     * 1) 参数区写 UPGRADE 状态 —— BOOT 检测后进入升级模式接收固件
     *   （独立状态，不会误判为回滚）；
     * 2) BKP 标志（HAL 索引已修正）作为直接触发。 */
    ota_param_t param;
    memcpy(&param, (const void *)OTA_PARAM_ADDR, sizeof(param));
    if (param.magic != OTA_PARAM_MAGIC || param.crc32 != ota_param_crc(&param)) {
        memset(&param, 0, sizeof(param));
        param.magic = OTA_PARAM_MAGIC;
    }
    param.boot_state = OTA_STATE_UPGRADE;
    param.boot_count = 0;
    param.last_error = 0;
    param.crc32 = ota_param_crc(&param);
    ota_flash_erase(OTA_PARAM_ADDR, 0);
    ota_flash_write(OTA_PARAM_ADDR, (const uint8_t *)&param, sizeof(param));
    ota_flash_write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                    (const uint8_t *)&param, sizeof(param));

    BSP_RTC_WriteBackupReg(0, BOOT_FLAG_UPGRADE);
    BSP_DelayMs(100);
    BSP_SystemReset();
    return 0;   /* 不会到达 */
}

uint8_t Ota_Status(uint8_t *state, uint32_t *received, uint32_t *total)
{
    *state = ota_state;
    *received = ota_received;
    *total = ota_total;
    return 0;
}

void OtaAgent_Init(void)
{
    EventBus_Subscribe(MSG_CMD_OTA_START, handle_ota_start_msg);
    ota_confirm_startup();
    LOG_Printf("OTA Agent ready (download@0x%08X).\r\n",
               (unsigned)OTA_DOWNLOAD_ADDR);
}
