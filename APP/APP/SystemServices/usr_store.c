/* ================================================================
 * 用户数据统一存储实现（日志式 EEPROM，见 usr_store.h）
 * ================================================================ */
#include "usr_store.h"
#include "bsp_eeprom.h"
#include "crc16.h"
#include "logger.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <string.h>

#define USR_MAGIC       0xA5u
#define USR_TOMB_LEN    0xFFu
#define USR_HDR_SIZE    5u          /* magic + key + len + crc16 */

typedef struct {
    uint16_t off;
    uint8_t  key;
    uint8_t  len;                   /* 0xFF = 墓碑（删除标记） */
    uint16_t crc;
    uint8_t  valid;
} usr_rec_t;

static SemaphoreHandle_t s_lock = NULL;
static uint8_t s_valid = 0;

/* compact 静态暂存（互斥保护，单实例，避免任务栈压力） */
static uint16_t s_rec_off[256];
static uint8_t  s_rec_len[256];
static uint8_t  s_rec_have[256];

/* CRC 覆盖 magic(1)+key(1)+len(1)+data(len) */
static uint16_t rec_crc(uint8_t key, uint8_t len, const uint8_t *data)
{
    uint8_t buf[3u + USR_DATA_MAX];
    buf[0] = USR_MAGIC;
    buf[1] = key;
    buf[2] = len;
    if (len != USR_TOMB_LEN && len > 0u && data != NULL) {
        memcpy(buf + 3, data, len);
    }
    return CRC16_Calculate(buf, (uint16_t)(3u + ((len == USR_TOMB_LEN) ? 0u : len)));
}

/* 读取 off 处记录头；返回记录总长（含头）；
 * 0 = 日志干净结束（0xFF/越界），0xFFFF = 损坏（触发重格式化） */
static uint16_t rec_scan(uint16_t off, usr_rec_t *r)
{
    uint8_t hdr[USR_HDR_SIZE];
    if (off + USR_HDR_SIZE > USR_EEPROM_SIZE) {
        return 0;
    }
    if (BSP_EEPROM_Read(off, hdr, USR_HDR_SIZE) != 0) {
        return 0;
    }
    if (hdr[0] != USR_MAGIC) {
        return (hdr[0] == 0xFFu) ? 0u : 0xFFFFu;   /* 空闲 vs 垃圾字节 */
    }

    r->off = off;
    r->key = hdr[1];
    r->len = hdr[2];
    r->crc = (uint16_t)((uint16_t)hdr[3] << 8 | hdr[4]);

    uint16_t dlen = (r->len == USR_TOMB_LEN) ? 0u : r->len;
    uint16_t total = (uint16_t)(USR_HDR_SIZE + dlen);
    if (off + total > USR_EEPROM_SIZE) {
        return 0xFFFFu;                            /* 长度越界 = 损坏 */
    }

    uint8_t data[USR_DATA_MAX];
    if (dlen > 0u &&
        BSP_EEPROM_Read((uint16_t)(off + USR_HDR_SIZE), data, dlen) != 0) {
        return 0xFFFFu;
    }
    r->valid = (rec_crc(r->key, r->len, data) == r->crc) ? 1u : 0u;
    return total;
}

/* 日志写入位置（第一个空闲偏移） */
static uint16_t log_append_off(void)
{
    uint16_t off = 0;
    while (off < USR_EEPROM_SIZE) {
        usr_rec_t r;
        uint16_t total = rec_scan(off, &r);
        if (total == 0u || total == 0xFFFFu) {
            break;
        }
        off = (uint16_t)(off + total);
    }
    return off;
}

/* 单遍扫描：返回 key 最新有效记录（-1 无，-2 墓碑，0=有）
 * data 缓冲由调用方提供（≥USR_DATA_MAX） */
static int rec_find_latest(uint8_t key, usr_rec_t *out, uint8_t *data)
{
    int found = -1;
    uint16_t off = 0;
    while (off < USR_EEPROM_SIZE) {
        usr_rec_t r;
        uint16_t total = rec_scan(off, &r);
        if (total == 0u || total == 0xFFFFu) {
            break;
        }
        if (r.valid && r.key == key) {
            out->off = r.off;
            out->len = r.len;
            out->crc = r.crc;
            out->valid = 1u;
            if (r.len == USR_TOMB_LEN) {
                found = -2;
            } else if (BSP_EEPROM_Read((uint16_t)(r.off + USR_HDR_SIZE),
                                       data, r.len) == 0) {
                found = 0;
            } else {
                return -3;
            }
        }
        off = (uint16_t)(off + total);
    }
    return found;
}

/* 整片擦除（写 0xFF）并重写各 key 最新值（日志紧凑） */
static int store_compact(void)
{
    uint16_t off = 0;
    memset(s_rec_have, 0, sizeof(s_rec_have));
    while (off < USR_EEPROM_SIZE) {
        usr_rec_t r;
        uint16_t total = rec_scan(off, &r);
        if (total == 0u || total == 0xFFFFu) {
            break;
        }
        if (r.valid && r.key != 0xFFu) {
            if (r.len == USR_TOMB_LEN) {
                s_rec_have[r.key] = 0;             /* 墓碑：移除 */
            } else {
                s_rec_off[r.key] = r.off;
                s_rec_len[r.key] = r.len;
                s_rec_have[r.key] = 1;
            }
        }
        off = (uint16_t)(off + total);
    }

    uint8_t out[USR_EEPROM_SIZE];
    uint16_t n = 0;
    for (uint16_t key = 1; key < 256u; key++) {
        if (!s_rec_have[key]) {
            continue;
        }
        uint8_t data[USR_DATA_MAX];
        uint16_t dlen = s_rec_len[key];
        if (BSP_EEPROM_Read((uint16_t)(s_rec_off[key] + USR_HDR_SIZE),
                            data, dlen) != 0) {
            return -2;
        }
        uint8_t hdr[USR_HDR_SIZE];
        hdr[0] = USR_MAGIC;
        hdr[1] = (uint8_t)key;
        hdr[2] = (uint8_t)dlen;
        uint16_t crc = rec_crc((uint8_t)key, (uint8_t)dlen, data);
        hdr[3] = (uint8_t)(crc >> 8);
        hdr[4] = (uint8_t)(crc & 0xFF);
        if (n + USR_HDR_SIZE + dlen > USR_EEPROM_SIZE) {
            return -3;                               /* 总数据超容量 */
        }
        memcpy(out + n, hdr, USR_HDR_SIZE);
        if (dlen > 0u) {
            memcpy(out + n + USR_HDR_SIZE, data, dlen);
        }
        n = (uint16_t)(n + USR_HDR_SIZE + dlen);
    }

    uint8_t ff[USR_EEPROM_SIZE];
    memset(ff, 0xFF, sizeof(ff));
    if (BSP_EEPROM_Write(0, ff, USR_EEPROM_SIZE) != 0) {
        return -4;
    }
    if (n > 0u && BSP_EEPROM_Write(0, out, n) != 0) {
        return -5;
    }
    return 0;
}

int UsrStore_Valid(void)
{
    return s_valid ? 1 : 0;
}

int UsrStore_Get(uint8_t key, void *out, uint16_t max_len)
{
    if (!s_valid || out == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return -4;
    }
    uint8_t data[USR_DATA_MAX];
    usr_rec_t r;
    int rc = rec_find_latest(key, &r, data);
    int ret = -1;
    if (rc == 0) {
        if (r.len > max_len) {
            ret = -2;
        } else {
            memcpy(out, data, r.len);
            ret = r.len;
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

int UsrStore_Set(uint8_t key, const void *data, uint16_t len)
{
    if (!s_valid || data == NULL || len > USR_DATA_MAX || key == 0xFFu) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -4;
    }
    uint8_t hdr[USR_HDR_SIZE];
    uint16_t off = 0;
    uint16_t crc = 0;
    int ret = -1;
    uint16_t need = (uint16_t)(USR_HDR_SIZE + len);
    if (log_append_off() + need > USR_EEPROM_SIZE) {
        if (store_compact() != 0) {
            goto out;
        }
    }
    off = log_append_off();
    if (off + need > USR_EEPROM_SIZE) {
        goto out;                                   /* 单记录仍放不下 */
    }

    hdr[0] = USR_MAGIC;
    hdr[1] = key;
    hdr[2] = (uint8_t)len;
    hdr[3] = 0;
    hdr[4] = 0;
    crc = rec_crc(key, (uint8_t)len, (const uint8_t *)data);
    hdr[3] = (uint8_t)(crc >> 8);
    hdr[4] = (uint8_t)(crc & 0xFF);
    if (BSP_EEPROM_Write(off, hdr, USR_HDR_SIZE) != 0) {
        goto out;
    }
    if (len > 0u &&
        BSP_EEPROM_Write((uint16_t)(off + USR_HDR_SIZE),
                         (const uint8_t *)data, len) != 0) {
        goto out;
    }
    ret = 0;
out:
    xSemaphoreGive(s_lock);
    return ret;
}

int UsrStore_Erase(uint8_t key)
{
    if (!s_valid || key == 0xFFu) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -4;
    }
    uint8_t hdr[USR_HDR_SIZE];
    uint16_t off = 0;
    uint16_t crc = 0;
    int ret = -1;
    if (log_append_off() + USR_HDR_SIZE > USR_EEPROM_SIZE) {
        if (store_compact() != 0) {
            goto out;
        }
    }
    off = log_append_off();
    if (off + USR_HDR_SIZE > USR_EEPROM_SIZE) {
        goto out;
    }
    hdr[0] = USR_MAGIC;
    hdr[1] = key;
    hdr[2] = USR_TOMB_LEN;
    hdr[3] = 0;
    hdr[4] = 0;
    crc = rec_crc(key, USR_TOMB_LEN, NULL);
    hdr[3] = (uint8_t)(crc >> 8);
    hdr[4] = (uint8_t)(crc & 0xFF);
    ret = (BSP_EEPROM_Write(off, hdr, USR_HDR_SIZE) == 0) ? 0 : -1;
out:
    xSemaphoreGive(s_lock);
    return ret;
}

int UsrStore_Reset(void)
{
    if (!s_valid) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -4;
    }
    uint8_t ff[USR_EEPROM_SIZE];
    memset(ff, 0xFF, sizeof(ff));
    int ret = (BSP_EEPROM_Write(0, ff, USR_EEPROM_SIZE) == 0) ? 0 : -1;
    xSemaphoreGive(s_lock);
    return ret;
}

uint32_t UsrStore_Count(void)
{
    uint8_t seen[32];
    memset(seen, 0, sizeof(seen));
    if (!s_valid || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return 0;
    }
    uint16_t off = 0;
    while (off < USR_EEPROM_SIZE) {
        usr_rec_t r;
        uint16_t total = rec_scan(off, &r);
        if (total == 0u || total == 0xFFFFu) {
            break;
        }
        if (r.valid && r.key != 0xFFu) {
            if (r.len == USR_TOMB_LEN) {
                seen[r.key >> 3] &= (uint8_t)~(1u << (r.key & 7u));
            } else {
                seen[r.key >> 3] |= (uint8_t)(1u << (r.key & 7u));
            }
        }
        off = (uint16_t)(off + total);
    }
    xSemaphoreGive(s_lock);
    uint32_t n = 0;
    for (uint32_t i = 0; i < sizeof(seen); i++) {
        for (uint32_t b = 0; b < 8u; b++) {
            if (seen[i] & (1u << b)) {
                n++;
            }
        }
    }
    return n;
}

void UsrStore_Info(uint32_t *used, uint32_t *free)
{
    uint32_t u = 0;
    if (s_valid && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        u = log_append_off();
        xSemaphoreGive(s_lock);
    }
    if (used != NULL) {
        *used = u;
    }
    if (free != NULL) {
        *free = (u <= USR_EEPROM_SIZE) ? (USR_EEPROM_SIZE - u) : 0u;
    }
}

void UsrStore_Init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_valid = 0;
    if (BSP_EEPROM_Init() != 0) {
        LOG_Printf("[USR] EEPROM not present (soft IIC PB8/PB9 @0x50)\r\n");
        return;
    }

    /* 日志完整性校验：坏首字节/中途损坏 → 自动重格式化 */
    uint16_t off = 0;
    int bad = 0;
    while (off < USR_EEPROM_SIZE) {
        usr_rec_t r;
        uint16_t total = rec_scan(off, &r);
        if (total == 0u) {
            break;                                  /* 干净结束 */
        }
        if (total == 0xFFFFu) {
            bad = 1;
            break;
        }
        off = (uint16_t)(off + total);
    }
    if (bad) {
        LOG_Printf("[USR] log corrupted, reformatting\r\n");
        uint8_t ff[USR_EEPROM_SIZE];
        memset(ff, 0xFF, sizeof(ff));
        if (BSP_EEPROM_Write(0, ff, USR_EEPROM_SIZE) != 0) {
            LOG_Printf("[USR] reformat FAILED\r\n");
            return;
        }
        off = 0;
    }
    s_valid = 1;
    LOG_Printf("[USR] ready (log %uB / %uB)\r\n",
               (unsigned)off, (unsigned)USR_EEPROM_SIZE);
}
