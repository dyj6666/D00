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
#include <stddef.h>

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
    uint32_t last_build_no;
    uint32_t crc32;
} ota_param_t;
#pragma pack()

/* ---------------- 断点续传会话（DOWNLOAD 区尾部槽区） ---------------- */
#pragma pack(1)
typedef struct {
    uint32_t magic;       /* OTA_SESSION_MAGIC */
    uint32_t version;
    uint32_t total;
    uint32_t received;
    uint32_t crc32;       /* 覆盖 magic..received */
} ota_session_t;          /* 20B，槽间距 32B */
#pragma pack()

static bool ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* ---------------- 内部状态 ---------------- */
static volatile uint8_t  ota_state = OTA_ST_IDLE;
static uint32_t ota_total = 0;        /* 固件总大小 */
static uint32_t ota_received = 0;     /* 已收字节 */
static uint32_t ota_begin_version = 0; /* 本次会话版本（会话槽持久化用） */

static uint32_t ota_session_crc(const ota_session_t *s)
{
    const uint8_t *d = (const uint8_t *)s;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < offsetof(ota_session_t, crc32); i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

static void ota_session_save(uint32_t slot, uint32_t version,
                             uint32_t total, uint32_t received)
{
    if (slot >= OTA_SESSION_SLOTS) return;
    ota_session_t s;
    s.magic = OTA_SESSION_MAGIC;
    s.version = version;
    s.total = total;
    s.received = received;
    s.crc32 = ota_session_crc(&s);
    bool ok = ota_flash_write(OTA_SESSION_BASE + slot * 32,
                              (const uint8_t *)&s, sizeof(s));
    if (!ok) {
        LOG_Printf("OTA: sess save FAIL slot=%lu addr=0x%08lX\r\n",
                   (unsigned long)slot,
                   (unsigned long)(OTA_SESSION_BASE + slot * 32));
    }
}

/* 扫描槽区，返回最新（槽号最大）的有效会话 */
static bool ota_session_latest(ota_session_t *out)
{
    ota_session_t best;
    memset(&best, 0, sizeof(best));
    for (uint32_t i = 0; i < OTA_SESSION_SLOTS; i++) {
        ota_session_t s;
        memcpy(&s, (const void *)(OTA_SESSION_BASE + i * 32), sizeof(s));
        if (s.magic != OTA_SESSION_MAGIC) continue;
        if (s.crc32 != ota_session_crc(&s)) continue;
        if (s.received > s.total) continue;
        best = s;
    }
    *out = best;
    return (best.magic == OTA_SESSION_MAGIC && best.received > 0 &&
            best.received < best.total);
}

/* 失效全部会话槽：魔数写 0（1→0，无需擦除），约几十毫秒 */
static void ota_session_clear(void)
{
    uint8_t zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < OTA_SESSION_SLOTS; i++) {
        (void)ota_flash_write(OTA_SESSION_BASE + i * 32, zero, sizeof(zero));
    }
}

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
/* 地址 → Flash 扇区号 */
static uint32_t ota_flash_sector_of(uint32_t addr)
{
    if (addr >= 0x08000000 && addr < 0x08004000) return FLASH_SECTOR_0;
    if (addr < 0x08008000) return FLASH_SECTOR_1;
    if (addr < 0x0800C000) return FLASH_SECTOR_2;
    if (addr < 0x08010000) return FLASH_SECTOR_3;
    if (addr < 0x08020000) return FLASH_SECTOR_4;
    if (addr < 0x08040000) return FLASH_SECTOR_5;
    if (addr < 0x08060000) return FLASH_SECTOR_6;
    if (addr < 0x08080000) return FLASH_SECTOR_7;
    if (addr < 0x080A0000) return FLASH_SECTOR_8;
    if (addr < 0x080C0000) return FLASH_SECTOR_9;
    if (addr < 0x080E0000) return FLASH_SECTOR_10;
    return FLASH_SECTOR_11;
}

/* 擦除 [addr, addr+len) 覆盖的全部扇区；len==0 时仅擦 addr 所在扇区。
 * DOWNLOAD 现为 256KB（扇区9+10），必须整区擦除后再写入，防止编程 0→1 失败。 */
static bool ota_flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t start_sector = ota_flash_sector_of(addr);
    uint32_t end_sector = (len == 0) ? start_sector
                                     : ota_flash_sector_of(addr + len - 1);
    for (uint32_t sector = start_sector; sector <= end_sector; sector++) {
        HAL_FLASH_Unlock();
        /* 清全部错误标志（含 OPTERR/SOP）：RDP 解除或此前操作可能残留，
         * HAL_FLASHEx_Erase 检测到错误会直接返回 HAL_ERROR */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR |
                               FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                               FLASH_FLAG_OPERR);
        FLASH_EraseInitTypeDef er = {
            .TypeErase = FLASH_TYPEERASE_SECTORS,
            .Sector = sector,
            .NbSectors = 1,
            .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        };
        uint32_t err = 0;
        HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &err);
        HAL_FLASH_Lock();
        LOG_Printf("OTA: flash erase sector=%lu st=%d err=0x%08lX\r\n",
                   (unsigned long)sector, (int)st, (unsigned long)err);
        if (st != HAL_OK || err != 0xFFFFFFFF) return false;
    }
    return true;
}

static bool ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) return true;
    __disable_irq();   /* Flash 编程序列必须原子执行，防中断打断 */
    HAL_FLASH_Unlock();

    /* 前导非对齐字节 */
    while ((addr & 0x03) && len > 0) {
        uint32_t wa = addr & ~0x03u;
        uint32_t val = *(volatile uint32_t *)wa;
        ((uint8_t *)&val)[addr & 0x03] = *data;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, wa, val) != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
    }
    while (len >= 4) {
        uint32_t word;
        memcpy(&word, data, 4);
        HAL_StatusTypeDef hs = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word);
        if (hs != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
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
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
    }
    HAL_FLASH_Lock();
    __enable_irq();
    return true;
}

static uint32_t ota_param_crc(const ota_param_t *p)
{
    const uint8_t *d = (const uint8_t *)p;
    uint32_t crc = 0xFFFFFFFFu;
    /* 只计算 crc32 字段之前的数据（避免 CRC 自引用导致校验恒失败） */
    for (uint32_t i = 0; i < offsetof(ota_param_t, crc32); i++) {
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
    bool e = ota_flash_erase(OTA_PARAM_ADDR, 0);
    bool w0 = ota_flash_write(OTA_PARAM_ADDR, (const uint8_t *)&p, sizeof(p));
    bool w1 = ota_flash_write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                              (const uint8_t *)&p, sizeof(p));
    LOG_Printf("OTA: confirm erase=%d write0=%d write1=%d\r\n",
               (int)e, (int)w0, (int)w1);
    if (e && w0 && w1) {
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
    ota_begin_version = version;
    LOG_Printf("OTA: begin v%lu size=%lu\r\n",
               (unsigned long)version, (unsigned long)size);

    /* 断点续传：存在有效会话且版本/大小匹配 → 不擦下载区，从断点继续 */
    ota_session_t sess;
    bool sess_ok = ota_session_latest(&sess);
    if (sess_ok &&
        sess.version == version && sess.total == size) {
        LOG_Printf("OTA: resume session from %lu/%lu\r\n",
                   (unsigned long)sess.received, (unsigned long)sess.total);
        /* 恢复路径不擦下载区：重置 Flash 控制器状态，避免残留导致编程 BSY 卡死 */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR |
                               FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                               FLASH_FLAG_OPERR);
        HAL_FLASH_Unlock();
        HAL_FLASH_Lock();
        ota_total = size;
        ota_received = sess.received;
        ota_state = OTA_ST_RECEIVING;
        return 0;
    }

    LOG_Printf("OTA: erasing download area...\r\n");
    if (!ota_flash_erase(OTA_DOWNLOAD_ADDR, OTA_DOWNLOAD_SIZE)) {
        LOG_Printf("OTA: download erase FAILED\r\n");
        return 3;
    }
    ota_total = size;
    ota_received = 0;
    ota_state = OTA_ST_RECEIVING;
    ota_session_save(0, version, size, 0);
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
        uint32_t probe = *(volatile uint32_t *)(OTA_DOWNLOAD_ADDR + offset);
        LOG_Printf("OTA: flash write FAILED at %lu probe=0x%08X SR=0x%08X\r\n",
                   (unsigned long)offset, (unsigned)probe,
                   (unsigned)(FLASH->SR));
        ota_state = OTA_ST_IDLE;
        return 3;
    }
    ota_received += len;
    /* 每块持久化一次精确进度（768 槽覆盖 ≤184KB；超出部分续传从最后有效槽恢复）：
     * 恢复点 = 实际已写位置，避免重写已写区域导致 Flash 编程失败 */
    uint32_t slot = ota_received / OTA_CHUNK_MAX;
    if (slot < OTA_SESSION_SLOTS) {
        ota_session_save(slot, ota_begin_version, ota_total, ota_received);
    }
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
    ota_session_save(OTA_SESSION_SLOTS - 1, ota_begin_version,
                     ota_total, ota_total);

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

uint8_t Ota_Reset(void)
{
    ota_state = OTA_ST_IDLE;
    ota_received = 0;
    ota_total = 0;
    ota_session_clear();
    LOG_Printf("OTA: session reset (fresh download required)\r\n");
    return 0;
}

void Ota_ForceRollbackTest(void)
{
    ota_param_t param;
    memcpy(&param, (const void *)OTA_PARAM_ADDR, sizeof(param));
    if (param.magic != OTA_PARAM_MAGIC || param.crc32 != ota_param_crc(&param)) {
        memset(&param, 0, sizeof(param));
        param.magic = OTA_PARAM_MAGIC;
    }
    param.boot_state = OTA_STATE_PENDING;
    param.boot_count = 3;      /* 与 BOOT MAX_BOOT_TRIES 一致：下次复位即回滚 */
    param.last_error = 0;
    param.crc32 = ota_param_crc(&param);
    ota_flash_erase(OTA_PARAM_ADDR, 0);
    ota_flash_write(OTA_PARAM_ADDR, (const uint8_t *)&param, sizeof(param));
    ota_flash_write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                    (const uint8_t *)&param, sizeof(param));
    LOG_Printf("OTA: rollback test armed (PENDING+MAX), resetting...\r\n");
    BSP_DelayMs(100);
    BSP_SystemReset();
}

void OtaAgent_Init(void)
{
    EventBus_Subscribe(MSG_CMD_OTA_START, handle_ota_start_msg);
    ota_confirm_startup();
    ota_param_t st;
    memcpy(&st, (const void *)OTA_PARAM_ADDR, sizeof(st));
    if (st.magic == OTA_PARAM_MAGIC && st.crc32 == ota_param_crc(&st)) {
        LOG_Printf("[APP] OTA  : Agent ready (last build %lu)\r\n",
                   (unsigned long)st.last_build_no);
    } else {
        LOG_Printf("[APP] OTA  : Agent ready (param invalid)\r\n");
    }
}
