/**
 * @file    boot_param.c
 * @brief   Flash 参数区实现：同一扇区内双份块，读时选 CRC 有效的一份，
 *          写时整扇区擦除后写两份，防止写入中途断电导致整区失效。
 */
#include "boot_param.h"
#include "flash_if.h"

#include <string.h>

uint32_t boot_param_crc(const boot_param_t *p)
{
    /* CRC-32 (IEEE)：轻量软件实现。必须按结构体实际大小计算，
     * 此前误用 PARAM_COPY_SIZE(32) 导致读到结构体后 8 字节越界垃圾，
     * 使参数区 CRC 校验永远失败、PENDING 状态无法持久化。 */
    const uint8_t *data = (const uint8_t *)p;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sizeof(boot_param_t); i++) {
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
    if (!flash_erase(PARAM_BASE_ADDR, PARAM_BASE_ADDR + PARAM_SIZE - 1)) {
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
    printf("[PARAM] save OK (state=%lu count=%lu)\r\n",
           (unsigned long)p.boot_state, (unsigned long)p.boot_count);
    return true;
}
