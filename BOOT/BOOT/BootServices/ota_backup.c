/* ================================================================
 * ota_backup —— BOOT 侧外部备份/回滚存储服务实现
 *
 * 依赖：esp_flash（SPI 驱动）、flash_if（内部 Flash 写）、boot_config
 * 原则：全量快照（APP_SIZE）而非增量；写后读回校验；逐块喂狗。
 * ================================================================ */
#include "ota_backup.h"
#include "esp_flash.h"
#include "flash_if.h"
#include "boot_config.h"
#include "iwdg.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* 数据区起始 = 备份槽 + 独立头扇区 */
#define BKUP_DATA_OFF   (ESP_BACKUP_BASE + ESP_BACKUP_HDR_OFF)

/* 内部 Flash 与外部 Flash 逐块搬运的块大小（栈友好，512B） */
#define BKUP_CHUNK      512u

static bool s_ready;                 /* OtaBackup_Init 探测结果 */
static uint8_t s_chunk[BKUP_CHUNK];  /* 单块搬运缓冲（BOOT 无 OS，静态可重入） */
static uint8_t s_cmp[BKUP_CHUNK];    /* 读回校验缓冲（避免大栈） */

/* ---------------- 通用 CRC32（IEEE 多项式，与 boot_param 一致） ----------------
 * 兼容性警告：无 final-xor 位算法，与 crc32.c（查表+final xor）语义不同；
 * 备份槽已有持久化头部依赖本实现，禁止替换（会令存量备份全部失效）。 */
static uint32_t bkup_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t m = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ m;
        }
    }
    return crc;
}

static void bkup_header_calc(ota_backup_header_t *h)
{
    /* CRC 覆盖 magic..reserved[3]（crc32 字段之前的所有字段） */
    h->crc32 = bkup_crc32((const uint8_t *)h,
                          offsetof(ota_backup_header_t, crc32));
}

static bool bkup_header_valid(const ota_backup_header_t *h)
{
    if (h->magic != OTA_BACKUP_MAGIC) {
        return false;
    }
    if (h->app_size != APP_SIZE) {
        return false;   /* 备份布局必须与当前分区一致 */
    }
    uint32_t calc = bkup_crc32((const uint8_t *)h,
                               offsetof(ota_backup_header_t, crc32));
    return (h->crc32 == calc);
}

/* ---------------- 生命周期 ---------------- */
bool OtaBackup_Init(void)
{
    s_ready = EspFlash_Init();
    if (!s_ready) {
        printf("[BKUP] External flash not ready, backup disabled\r\n");
    }
    return s_ready;
}

bool OtaBackup_IsValid(void)
{
    if (!s_ready) {
        return false;
    }
    ota_backup_header_t h;
    if (!EspFlash_Read(ESP_BACKUP_BASE, &h, sizeof(h))) {
        return false;
    }
    return bkup_header_valid(&h);
}

/* ---------------- 备份：RUN → 外部槽（写后读回校验） ---------------- */
bool OtaBackup_Save(void)
{
    if (!s_ready) {
        printf("[BKUP] Save aborted: ext flash offline\r\n");
        return false;
    }
    printf("[BKUP] Erasing backup area (%lu KB)...\r\n",
           (unsigned long)(ESP_BACKUP_SIZE / 1024u));
    if (!EspFlash_EraseRange64(ESP_BACKUP_BASE, ESP_BACKUP_SIZE)) {
        printf("[BKUP] Erase failed\r\n");
        return false;
    }

    /* 1) 备份头 */
    ota_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic    = OTA_BACKUP_MAGIC;
    h.app_size = APP_SIZE;
    h.build_no = *(volatile uint32_t *)APP_VERSION_ADDR;
    bkup_header_calc(&h);
    if (!EspFlash_Write(ESP_BACKUP_BASE, &h, sizeof(h))) {
        printf("[BKUP] Header write failed\r\n");
        return false;
    }

    /* 2) RUN 全量数据（内部 Flash 内存直读 → 外部写） */
    printf("[BKUP] Saving RUN (%lu KB)...\r\n",
           (unsigned long)(APP_SIZE / 1024u));
    for (uint32_t off = 0u; off < APP_SIZE; off += BKUP_CHUNK) {
        uint32_t n = APP_SIZE - off;
        if (n > BKUP_CHUNK) {
            n = BKUP_CHUNK;
        }
        memcpy(s_chunk, (const void *)(APP_BASE_ADDR + off), n);
        if (!EspFlash_Write(BKUP_DATA_OFF + off, s_chunk, n)) {
            printf("[BKUP] Data write failed @0x%08X\r\n", off);
            return false;
        }
        IWDG->KR = 0xAAAA;   /* 每块喂狗 */
    }

    /* 3) 写后读回校验：逐块比对，保证备份 100% 完整 */
    printf("[BKUP] Verifying backup...\r\n");
    for (uint32_t off = 0u; off < APP_SIZE; off += BKUP_CHUNK) {
        uint32_t n = APP_SIZE - off;
        if (n > BKUP_CHUNK) {
            n = BKUP_CHUNK;
        }
        if (!EspFlash_Read(BKUP_DATA_OFF + off, s_cmp, n)) {
            printf("[BKUP] Verify read failed @0x%08X\r\n", off);
            return false;
        }
        if (memcmp(s_cmp, (const void *)(APP_BASE_ADDR + off), n) != 0) {
            printf("[BKUP] Verify mismatch @0x%08X\r\n", off);
            return false;
        }
        IWDG->KR = 0xAAAA;
    }

    /* 4) 读回头确认 */
    ota_backup_header_t rh;
    if (!EspFlash_Read(ESP_BACKUP_BASE, &rh, sizeof(rh)) ||
        !bkup_header_valid(&rh)) {
        printf("[BKUP] Header verify failed\r\n");
        return false;
    }
    printf("[BKUP] Save OK (build=%lu)\r\n", (unsigned long)h.build_no);
    return true;
}

/* ---------------- 恢复：外部槽 → RUN ---------------- */
bool OtaBackup_Restore(void)
{
    if (!s_ready) {
        return false;
    }
    ota_backup_header_t h;
    if (!EspFlash_Read(ESP_BACKUP_BASE, &h, sizeof(h)) ||
        !bkup_header_valid(&h)) {
        printf("[RB] External backup invalid (magic/crc)\r\n");
        return false;
    }
    printf("[RB] Restoring RUN from external backup (build=%lu)...\r\n",
           (unsigned long)h.build_no);

    if (!flash_erase(APP_BASE_ADDR, APP_BASE_ADDR + APP_SIZE - 1)) {
        printf("[RB] RUN erase failed\r\n");
        return false;
    }
    for (uint32_t off = 0u; off < APP_SIZE; off += BKUP_CHUNK) {
        uint32_t n = APP_SIZE - off;
        if (n > BKUP_CHUNK) {
            n = BKUP_CHUNK;
        }
        if (!EspFlash_Read(BKUP_DATA_OFF + off, s_chunk, n) ||
            !flash_write(APP_BASE_ADDR + off, s_chunk, n)) {
            printf("[RB] Restore failed @0x%08X\r\n", off);
            return false;
        }
        IWDG->KR = 0xAAAA;
    }

    /* 补 RUN 尾部魔数 + 版本（数据区本身不含尾部 8 字节有效性） */
    uint32_t mg  = APP_VALID_MAGIC;
    uint32_t ver = h.build_no;
    if (!flash_write(APP_VALID_ADDR, (uint8_t *)&mg, sizeof(mg)) ||
        !flash_write(APP_VERSION_ADDR, (uint8_t *)&ver, sizeof(ver))) {
        printf("[RB] Magic write failed\r\n");
        return false;
    }
    printf("[RB] Restore OK\r\n");
    return true;
}

/* ---------------- 清空备份槽（升级成功/防重放） ---------------- */
void OtaBackup_Clear(void)
{
    if (!s_ready) {
        return;
    }
    (void)EspFlash_EraseRange64(ESP_BACKUP_BASE, ESP_BACKUP_SIZE);
    printf("[BKUP] External backup slot cleared\r\n");
}
