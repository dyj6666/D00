/* ================================================================
 * EEPROM 键值存储实现（AT24C02 布局见 kv_store.h）
 * ================================================================ */
#include "kv_store.h"
#include "bsp_eeprom.h"
#include "crc16.h"
#include "logger.h"

#include <string.h>

#define KV_HEAD_MAGIC      0x3153564Bu    /* 'KVS1' */
#define KV_HEAD_VERSION    1
#define KV_HEAD_SIZE       16
#define KV_SLOT_SIZE       24
#define KV_SLOT_COUNT      10
#define KV_DATA_MAX        20

typedef struct {
    uint8_t key;
    uint8_t len;
    uint16_t crc;
    uint8_t data[KV_DATA_MAX];
} kv_slot_t;

static uint8_t s_valid = 0;

static uint16_t kv_slot_crc(const kv_slot_t *s)
{
    uint8_t tmp[KV_DATA_MAX + 2];
    tmp[0] = s->key;
    tmp[1] = s->len;
    memcpy(tmp + 2, s->data, s->len);
    return CRC16_Calculate(tmp, (uint16_t)(s->len + 2));
}

static int kv_slot_read(kv_slot_t *s, uint32_t idx)
{
    return BSP_EEPROM_Read(KV_HEAD_SIZE + (uint16_t)(idx * KV_SLOT_SIZE),
                           (uint8_t *)s, KV_SLOT_SIZE);
}

static int kv_slot_write(const kv_slot_t *s, uint32_t idx)
{
    return BSP_EEPROM_Write(KV_HEAD_SIZE + (uint16_t)(idx * KV_SLOT_SIZE),
                            (const uint8_t *)s, KV_SLOT_SIZE);
}

static void kv_format(void)
{
    uint8_t hdr[KV_HEAD_SIZE];
    memset(hdr, 0xFF, sizeof(hdr));
    hdr[0] = (uint8_t)(KV_HEAD_MAGIC & 0xFF);
    hdr[1] = (uint8_t)((KV_HEAD_MAGIC >> 8) & 0xFF);
    hdr[2] = (uint8_t)((KV_HEAD_MAGIC >> 16) & 0xFF);
    hdr[3] = (uint8_t)((KV_HEAD_MAGIC >> 24) & 0xFF);
    hdr[4] = KV_HEAD_VERSION;
    (void)BSP_EEPROM_Write(0, hdr, sizeof(hdr));
}

void KV_Init(void)
{
    s_valid = 0;
    if (BSP_EEPROM_Init() != 0) {
        LOG_Printf("[KV] EEPROM not present (soft IIC PB8/PB9 @0x50)\r\n");
        return;
    }
    uint8_t hdr[KV_HEAD_SIZE];
    if (BSP_EEPROM_Read(0, hdr, sizeof(hdr)) != 0) {
        return;
    }
    uint32_t magic = (uint32_t)(hdr[0] | (hdr[1] << 8) |
                                (hdr[2] << 16) | (hdr[3] << 24));
    if (magic != KV_HEAD_MAGIC) {
        LOG_Printf("[KV] header invalid, formatting\r\n");
        kv_format();
    }
    s_valid = 1;
    LOG_Printf("[KV] ready (%u slots, %uB)\r\n",
               (unsigned)KV_SLOT_COUNT, (unsigned)BSP_EEPROM_SIZE);
}

int KV_Valid(void)
{
    return s_valid;
}

int KV_Get(uint8_t key, void *out, uint16_t max_len)
{
    if (!s_valid || out == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < KV_SLOT_COUNT; i++) {
        kv_slot_t s;
        if (kv_slot_read(&s, i) != 0) {
            return -1;
        }
        if (s.key == 0xFF) {
            continue;
        }
        if (s.key == key && s.len <= KV_DATA_MAX &&
            kv_slot_crc(&s) == s.crc) {
            if (s.len > max_len) {
                return -2;
            }
            memcpy(out, s.data, s.len);
            return s.len;
        }
    }
    return -1;
}

int KV_Set(uint8_t key, const void *data, uint16_t len)
{
    if (!s_valid || data == NULL || len > KV_DATA_MAX) {
        return -1;
    }
    uint32_t empty = KV_SLOT_COUNT;
    for (uint32_t i = 0; i < KV_SLOT_COUNT; i++) {
        kv_slot_t s;
        if (kv_slot_read(&s, i) != 0) {
            return -1;
        }
        if (s.key == key) {
            s.len = (uint8_t)len;
            memcpy(s.data, data, len);
            s.crc = kv_slot_crc(&s);
            return kv_slot_write(&s, i);      /* 就地更新 */
        }
        if (s.key == 0xFF && empty == KV_SLOT_COUNT) {
            empty = i;
        }
    }
    if (empty == KV_SLOT_COUNT) {
        return -2;                            /* 槽满 */
    }
    kv_slot_t s;
    memset(&s, 0xFF, sizeof(s));
    s.key = key;
    s.len = (uint8_t)len;
    memcpy(s.data, data, len);
    s.crc = kv_slot_crc(&s);
    return kv_slot_write(&s, empty);
}

int KV_Erase(uint8_t key)
{
    if (!s_valid) {
        return -1;
    }
    for (uint32_t i = 0; i < KV_SLOT_COUNT; i++) {
        kv_slot_t s;
        if (kv_slot_read(&s, i) != 0) {
            return -1;
        }
        if (s.key == key) {
            kv_slot_t e;
            memset(&e, 0xFF, sizeof(e));
            return kv_slot_write(&e, i);
        }
    }
    return 0;
}

int KV_Reset(void)
{
    if (!s_valid) {
        return -1;
    }
    uint8_t ff[BSP_EEPROM_SIZE];
    memset(ff, 0xFF, sizeof(ff));
    if (BSP_EEPROM_Write(0, ff, sizeof(ff)) != 0) {
        return -1;
    }
    kv_format();
    return 0;
}

uint32_t KV_Count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < KV_SLOT_COUNT; i++) {
        kv_slot_t s;
        if (kv_slot_read(&s, i) == 0 && s.key != 0xFF &&
            s.len <= KV_DATA_MAX && kv_slot_crc(&s) == s.crc) {
            n++;
        }
    }
    return n;
}
