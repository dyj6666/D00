/**
 * @file    boot_param.c
 * @brief   Flash 参数区实现：同一扇区内双份块，读时选 CRC 有效的一份，
 *          写时整扇区擦除后写两份，防止写入中途断电导致整区失效。
 */
#include "boot_param.h"
#include "flash_if.h"
#include "stm32f4xx_hal_flash_ex.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* 参数扇区擦除：HAL 实现（设置 VoltageRange），与 APP 侧一致。
 * 注意：不在 BOOT 启动早期调用（早期擦除参数扇区实测会 Flash BSY 卡死），
 * 统一在进入升级模式后（延迟执行）或 param 分支使用。 */
static bool hal_erase_sector(uint32_t addr)
{
    uint32_t sector = FLASH_SECTOR_0;
    if (addr >= 0x08004000u) sector = FLASH_SECTOR_1;
    if (addr >= 0x08008000u) sector = FLASH_SECTOR_2;
    if (addr >= 0x0800C000u) sector = FLASH_SECTOR_3;
    if (addr >= 0x08010000u) sector = FLASH_SECTOR_4;
    if (addr >= 0x08020000u) sector = FLASH_SECTOR_5;
    if (addr >= 0x08040000u) sector = FLASH_SECTOR_6;
    if (addr >= 0x08060000u) sector = FLASH_SECTOR_7;
    if (addr >= 0x08080000u) sector = FLASH_SECTOR_8;
    if (addr >= 0x080A0000u) sector = FLASH_SECTOR_9;
    if (addr >= 0x080C0000u) sector = FLASH_SECTOR_10;
    if (addr >= 0x080E0000u) sector = FLASH_SECTOR_11;

    HAL_FLASH_Unlock();
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
    return (st == HAL_OK);
}

uint32_t boot_param_crc(const boot_param_t *p)
{
    /* CRC-32 (IEEE)：只计算 crc32 字段之前的数据字段。
     * 若把 crc32 字段自身纳入计算，save 基于旧值算 CRC、写入新值后
     * load 再用新值重算必然不等，参数区永远校验失败（PENDING 不持久化）。 */
    const uint8_t *data = (const uint8_t *)p;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < offsetof(boot_param_t, crc32); i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t m = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ m;
        }
    }
    return crc;
}

static bool param_valid(const boot_param_t *p)
{
    if (p->magic != PARAM_MAGIC) return false;
    if (p->crc32 != boot_param_crc(p)) return false;
    if (p->boot_state != BOOT_STATE_NORMAL &&
        p->boot_state != BOOT_STATE_PENDING &&
        p->boot_state != BOOT_STATE_RECOVERY &&
        p->boot_state != BOOT_STATE_UPGRADE) return false;
    return true;
}

void boot_param_defaults(boot_param_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = PARAM_MAGIC;
    p->boot_state = BOOT_STATE_NORMAL;
    p->boot_count = 0;
    p->rollback_count = 0;
    p->last_error = 0;
    p->crc32 = boot_param_crc(p);
}

void boot_param_load(boot_param_t *out)
{
    boot_param_t a, b;
    memcpy(&a, (const void *)PARAM_BASE_ADDR, sizeof(a));
    memcpy(&b, (const void *)(PARAM_BASE_ADDR + PARAM_SLOT_OFFSET), sizeof(b));

    bool a_ok = param_valid(&a);
    bool b_ok = param_valid(&b);
    if (a_ok && b_ok) {
        *out = a;   /* 双份均有效：取第一份 */
        return;
    }
    if (a_ok) { *out = a; return; }
    if (b_ok) { *out = b; return; }
    boot_param_defaults(out);
}

bool boot_param_save(const boot_param_t *in)
{
    boot_param_t p = *in;
    p.crc32 = boot_param_crc(&p);

    /* 整扇区擦除（扇区11），再写两份 */
    if (!hal_erase_sector(PARAM_BASE_ADDR)) {
        printf("[PARAM] erase FAIL\r\n");
        return false;
    }
    if (!flash_write(PARAM_BASE_ADDR, (const uint8_t *)&p, sizeof(p))) {
        printf("[PARAM] write slot0 FAIL\r\n");
        return false;
    }
    if (!flash_write(PARAM_BASE_ADDR + PARAM_SLOT_OFFSET,
                     (const uint8_t *)&p, sizeof(p))) {
        printf("[PARAM] write slot1 FAIL\r\n");
        return false;
    }
    /* 回读验证：确认写入真实持久化（排查"假写成功"） */
    boot_param_t chk;
    memcpy(&chk, (const void *)PARAM_BASE_ADDR, sizeof(chk));
    printf("[PARAM] verify slot0: magic=0x%08X state=%lu count=%lu crc=0x%08X/0x%08X\r\n",
           (unsigned)chk.magic,
           (unsigned long)chk.boot_state, (unsigned long)chk.boot_count,
           (unsigned)chk.crc32, (unsigned)p.crc32);
    if (chk.magic != PARAM_MAGIC || chk.boot_state != p.boot_state) {
        printf("[PARAM] VERIFY FAIL: 写入未持久化!\r\n");
        return false;
    }
    printf("[PARAM] save OK (state=%lu count=%lu)\r\n",
           (unsigned long)p.boot_state, (unsigned long)p.boot_count);
    return true;
}
