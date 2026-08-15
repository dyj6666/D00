/* ================================================================
 * ext_store —— 外部 Flash 顶级存储服务层实现
 *
 * 分区布局（W25Q128 16MB，4KB 扇区对齐）：
 *   [OTA_DL ] 0x000000 2MB   OTA 下载槽 ×4（512KB/槽）
 *   [IMG_LIB] 0x200000 2MB   固件镜像槽 ×3 + 索引
 *   [FS     ] 0x400000 8MB   LittleFS 预留
 *   [USER   ] 0xC00000 3MB   用户数据
 *   [META   ] 0xF00000 1MB   分区表镜像 ×2 / 坏区表 ×2 / 会话预留
 *
 * 可靠性设计：
 *   - 坏区表：4KB 扇区位图（4096bit=512B）双份存 META，CRC 保护；
 *     擦除失败自动标记，后续擦/写自动规避
 *   - 掉电安全：WriteSafe 双份（主/副）交替写，主为提交点，CRC 校验
 *   - 磨损均衡：双份固定交替，写寿命天然均衡
 * ================================================================ */
#include "ext_store.h"
#include "bsp_w25q128.h"
#include "logger.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <string.h>

/* ---------------- 常量 ---------------- */
#define EXT_SECTOR            4096u
#define EXT_BLOCK             65536u
#define EXT_BITMAP_WORDS      128u          /* 16MB/4KB = 4096bit = 512B */
#define EXT_BAD_MAGIC         0x4241444Du   /* 'BADM' */
#define EXT_SAFE_MAGIC        0x45584653u   /* 'EXFS' */
#define EXT_SAFE_HEADER       12u           /* 结构头 8B(magic+len+ver) + CRC 4B */

/* META 区内部布局（1MB） */
#define EXT_META_PART_A       0x000000u
#define EXT_META_PART_B       0x001000u
#define EXT_META_BAD_A        0x002000u
#define EXT_META_BAD_B        0x003000u

/* ---------------- 分区表（16MB 静态划分） ---------------- */
static const ext_part_desc_t s_parts[EXT_PART_COUNT] = {
    { "ota_dl",  0x000000u, 2u * 1024u * 1024u, EXT_SECTOR,
      EXT_PART_F_WRITABLE },
    { "img_lib", 0x200000u, 2u * 1024u * 1024u, EXT_SECTOR,
      EXT_PART_F_WRITABLE | EXT_PART_F_BOOT_CRIT },
    { "fs",      0x400000u, 8u * 1024u * 1024u, EXT_SECTOR,
      EXT_PART_F_WRITABLE },
    { "user",    0xC00000u, 3u * 1024u * 1024u, EXT_SECTOR,
      EXT_PART_F_WRITABLE },
    { "meta",    0xF00000u, 1u * 1024u * 1024u, EXT_SECTOR,
      EXT_PART_F_WRITABLE | EXT_PART_F_BOOT_CRIT },
};

/* 坏区表：4096bit 位图 + 头（520B，存 4KB 扇区） */
typedef struct {
    uint32_t magic;
    uint32_t crc32;               /* 覆盖 magic..bitmap 全部 */
    uint32_t bitmap[EXT_BITMAP_WORDS];
} ext_bad_map_t;

static uint8_t s_ready;           /* Init 成功标志 */
/* 双份读写帧缓冲（静态，避免任务栈压力；最大承载 512B 业务帧） */
static uint8_t s_safe_frame[512 + EXT_SAFE_HEADER];
static uint8_t s_safe_check[512 + EXT_SAFE_HEADER];

/* 业务序列互斥：WriteSafe/ReadSafe/坏区表更新均为多步序列 +
 * 共享静态缓冲，并发会互相踩踏。递归互斥允许 API 内嵌套
 * （如 WriteSafe 失败时调用 MarkBad 标记坏区）。 */
static SemaphoreHandle_t s_mutex;
#define EXT_LOCK_TIMEOUT_MS   5000u

static bool ext_lock(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTakeRecursive(s_mutex,
                                   pdMS_TO_TICKS(EXT_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void ext_unlock(void)
{
    (void)xSemaphoreGiveRecursive(s_mutex);
}

/* ---------------- CRC32（标准 0xEDB88320） ---------------- */
static uint32_t ext_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

/* 掉电安全帧结构（前置声明供锁内函数原型使用） */
typedef struct {
    uint32_t magic;
    uint16_t len;
    uint16_t ver;
    uint8_t  data[];              /* 数据 + 4B CRC32 尾部 */
} ext_safe_frame_t;

static int ext_writesafe_locked(ext_part_id_t id, uint32_t phys,
                                uint32_t stride, const void *data, uint32_t len);
static int ext_erase_range_locked(ext_part_id_t id, uint32_t off, uint32_t len);

/* ---------------- BSP 错误码映射 ---------------- */
static int ext_map_err(int rc){
    switch (rc) {
    case BSP_W25Q_OK:          return EXT_STORE_OK;
    case BSP_W25Q_ERR_PARAM:   return EXT_STORE_ERR_PARAM;
    case BSP_W25Q_ERR_INIT:    return EXT_STORE_ERR_INIT;
    case BSP_W25Q_ERR_BUSY:    return EXT_STORE_ERR_BUSY;
    default:                   return EXT_STORE_ERR_IO;
    }
}

/* ---------------- 分区边界校验 ---------------- */
static int ext_check_range(ext_part_id_t id, uint32_t off, uint32_t len)
{
    if (s_ready == 0u) {
        return EXT_STORE_ERR_INIT;
    }
    if (id >= EXT_PART_COUNT || len == 0u) {
        return EXT_STORE_ERR_PARAM;
    }
    const ext_part_desc_t *p = &s_parts[id];
    if (off >= p->size || len > (p->size - off)) {
        return EXT_STORE_ERR_PARAM;
    }
    return EXT_STORE_OK;
}

/* ---------------- 坏区表读写（双份 + CRC） ---------------- */
static int bad_map_read(ext_bad_map_t *out)
{
    ext_bad_map_t tmp;
    /* 先读主（A），无效再读副（B）；双份均无效按"全新全好"处理 */
    for (int copy = 0; copy < 2; copy++) {
        uint32_t base = (copy == 0) ? EXT_META_BAD_A : EXT_META_BAD_B;
        int rc = BSP_W25Q128_Read(s_parts[EXT_PART_META].base + base,
                                  (uint8_t *)&tmp, sizeof(tmp));
        if (rc == BSP_W25Q_OK && tmp.magic == EXT_BAD_MAGIC) {
            /* CRC 计算：crc32 字段先清零（写入时同样），覆盖全表 */
            uint32_t saved = tmp.crc32;
            tmp.crc32 = 0u;
            if (saved == ext_crc32((const uint8_t *)&tmp, sizeof(tmp))) {
                *out = tmp;
                return EXT_STORE_OK;
            }
        }
    }
    memset(out, 0, sizeof(*out));
    out->magic = EXT_BAD_MAGIC;
    return EXT_STORE_OK;
}

static int bad_map_write(const ext_bad_map_t *in)
{
    ext_bad_map_t tmp = *in;
    tmp.crc32 = 0u;   /* 先清零再整体算 CRC（读侧同规则） */
    tmp.crc32 = ext_crc32((const uint8_t *)&tmp, sizeof(tmp));
    /* 主/副各擦各写（掉电任一份完整即可） */
    for (int copy = 0; copy < 2; copy++) {
        uint32_t base = (copy == 0) ? EXT_META_BAD_A : EXT_META_BAD_B;
        uint32_t abs = s_parts[EXT_PART_META].base + base;
        int erc = BSP_W25Q128_EraseSector(abs);
        int wrc = BSP_W25Q128_Write(abs, (const uint8_t *)&tmp, sizeof(tmp));
        if (erc != BSP_W25Q_OK || wrc != BSP_W25Q_OK) {
            LOG_Printf("[EXT] badmap write FAIL copy=%d erase=%d write=%d\r\n",
                       copy, erc, wrc);
            return EXT_STORE_ERR_IO;
        }
    }
    return EXT_STORE_OK;
}

bool ExtStore_IsBad(ext_part_id_t id, uint32_t off)
{
    if (id >= EXT_PART_COUNT || s_ready == 0u) {
        return false;
    }
    const ext_part_desc_t *p = &s_parts[id];
    if (off >= p->size) {
        return true;
    }
    uint32_t sector = (p->base + off) / EXT_SECTOR;   /* 全片扇区号 */
    ext_bad_map_t map;
    if (bad_map_read(&map) != EXT_STORE_OK) {
        return false;
    }
    return (map.bitmap[sector >> 5] & (1u << (sector & 31u))) != 0u;
}

void ExtStore_MarkBad(ext_part_id_t id, uint32_t off)
{
    if (id >= EXT_PART_COUNT || s_ready == 0u) {
        return;
    }
    const ext_part_desc_t *p = &s_parts[id];
    if (off >= p->size) {
        return;
    }
    if (!ext_lock()) {
        LOG_Printf("[EXT] MarkBad lock timeout\r\n");
        return;
    }
    uint32_t sector = (p->base + off) / EXT_SECTOR;
    ext_bad_map_t map;
    if (bad_map_read(&map) != EXT_STORE_OK) {
        ext_unlock();
        return;
    }
    map.bitmap[sector >> 5] |= (1u << (sector & 31u));
    if (bad_map_write(&map) == EXT_STORE_OK) {
        LOG_Printf("[EXT] bad sector marked: %s @0x%lX (sec=%lu)\r\n",
                   p->name, (unsigned long)off,
                   (unsigned long)sector);
    }
    ext_unlock();
}

void ExtStore_BadMapClear(void)
{
    if (s_ready == 0u) {
        return;
    }
    if (!ext_lock()) {
        LOG_Printf("[EXT] BadMapClear lock timeout\r\n");
        return;
    }
    /* 擦坏区表双份扇区（恢复全好） */
    for (int copy = 0; copy < 2; copy++) {
        uint32_t base = (copy == 0) ? EXT_META_BAD_A : EXT_META_BAD_B;
        (void)BSP_W25Q128_EraseSector(s_parts[EXT_PART_META].base + base);
    }
    ext_unlock();
    LOG_Printf("[EXT] bad map cleared\r\n");
}

/* ---------------- 生命周期 ---------------- */
void ExtStore_Init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
    }
    int rc = ExtStore_Probe();
    if (rc == EXT_STORE_OK) {
        s_ready = 1u;
        LOG_Printf("[EXT] store ready: %lu KB, %u partitions\r\n",
                   (unsigned long)(BSP_W25Q_SIZE / 1024u),
                   (unsigned)EXT_PART_COUNT);
    } else {
        LOG_Printf("[EXT] store init FAIL rc=%d (ext flash offline?)\r\n", rc);
    }
}

int ExtStore_Probe(void)
{
    if (BSP_W25Q128_Probe() != BSP_W25Q_OK) {
        return EXT_STORE_ERR_IO;
    }
    return EXT_STORE_OK;
}

const ext_part_desc_t *ExtStore_GetPart(ext_part_id_t id)
{
    return (id < EXT_PART_COUNT) ? &s_parts[id] : NULL;
}

uint32_t ExtStore_Size(ext_part_id_t id)
{
    return (id < EXT_PART_COUNT) ? s_parts[id].size : 0u;
}

/* ---------------- 擦除：坏区自动跳过 ---------------- */
int ExtStore_Erase(ext_part_id_t id)
{
    return ExtStore_EraseRange(id, 0u, ExtStore_Size(id));
}

int ExtStore_EraseRange(ext_part_id_t id, uint32_t off, uint32_t len)
{
    int rc = ext_check_range(id, off, len);
    if (rc != EXT_STORE_OK) {
        return rc;
    }
    if ((s_parts[id].flags & EXT_PART_F_WRITABLE) == 0u) {
        return EXT_STORE_ERR_RO;
    }
    /* 多步擦除序列：持锁防并发（内部 MarkBad/IsBad 递归可重入） */
    if (!ext_lock()) {
        return EXT_STORE_ERR_BUSY;
    }
    rc = ext_erase_range_locked(id, off, len);
    ext_unlock();
    return rc;
}

static int ext_erase_range_locked(ext_part_id_t id, uint32_t off, uint32_t len)
{
    const ext_part_desc_t *p = &s_parts[id];
    uint32_t end = off + len;
    uint32_t pos = off;
    /* 优先 64KB 块擦：块内无坏区整块擦（快）；有坏区逐扇区擦并跳过坏区 */
    while (pos < end) {
        bool aligned64 = ((p->base + pos) % EXT_BLOCK) == 0u &&
                         (end - pos) >= EXT_BLOCK;
        bool has_bad = false;
        if (aligned64) {
            for (uint32_t o = pos; o < pos + EXT_BLOCK; o += EXT_SECTOR) {
                if (ExtStore_IsBad(id, o)) {
                    has_bad = true;
                    break;
                }
            }
        }
        if (aligned64 && !has_bad) {
            int erc = BSP_W25Q128_EraseBlock64(p->base + pos);
            if (erc != BSP_W25Q_OK) {
                ExtStore_MarkBad(id, pos);
                pos += EXT_SECTOR;
                continue;
            }
            pos += EXT_BLOCK;
            continue;
        }
        /* 逐 4KB 扇区擦：坏区跳过（不擦），失败标记后跳过 */
        uint32_t limit = aligned64 ? (pos + EXT_BLOCK) : end;
        while (pos < limit) {
            if (ExtStore_IsBad(id, pos)) {
                pos += EXT_SECTOR;         /* 坏区：跳过不擦 */
                continue;
            }
            if (BSP_W25Q128_EraseSector(p->base + pos) != BSP_W25Q_OK) {
                ExtStore_MarkBad(id, pos); /* 失败：标记后跳过 */
            }
            pos += EXT_SECTOR;
        }
    }
    return EXT_STORE_OK;
}

/* ---------------- 基础读写 ---------------- */
int ExtStore_Read(ext_part_id_t id, uint32_t off, void *buf, uint32_t len)
{
    int rc = ext_check_range(id, off, len);
    if (rc != EXT_STORE_OK) {
        return rc;
    }
    if (buf == NULL) {
        return EXT_STORE_ERR_PARAM;
    }
    return ext_map_err(BSP_W25Q128_Read(s_parts[id].base + off, buf, len));
}

int ExtStore_Write(ext_part_id_t id, uint32_t off, const void *data, uint32_t len)
{
    int rc = ext_check_range(id, off, len);
    if (rc != EXT_STORE_OK) {
        return rc;
    }
    if (data == NULL) {
        return EXT_STORE_ERR_PARAM;
    }
    if ((s_parts[id].flags & EXT_PART_F_WRITABLE) == 0u) {
        return EXT_STORE_ERR_RO;
    }
    if (!ext_lock()) {
        return EXT_STORE_ERR_BUSY;
    }
    /* 写入前检查目标扇区坏标记（坏区不写）；持锁保证 check-then-act 原子 */
    for (uint32_t o = off; o < off + len; o += EXT_SECTOR) {
        if (ExtStore_IsBad(id, o)) {
            ext_unlock();
            return EXT_STORE_ERR_BAD;
        }
    }
    rc = ext_map_err(BSP_W25Q128_Write(s_parts[id].base + off, data, len));
    ext_unlock();
    return rc;
}

/* ---------------- 掉电安全双份读写 ---------------- */
int ExtStore_WriteSafe(ext_part_id_t id, uint32_t slot, uint32_t stride,
                       const void *data, uint32_t len)
{
    /* stride 必须是 4KB 扇区整数倍：双份各占整扇区，擦/写地址才对齐 */
    if (stride < (len + EXT_SAFE_HEADER) || (stride % EXT_SECTOR) != 0u) {
        return EXT_STORE_ERR_PARAM;
    }
    uint32_t phys = slot * 2u * stride;
    int rc = ext_check_range(id, phys, 2u * stride);
    if (rc != EXT_STORE_OK) {
        return rc;
    }
    if (data == NULL || len == 0u || len > 0xFFFEu) {
        return EXT_STORE_ERR_PARAM;
    }
    if ((s_parts[id].flags & EXT_PART_F_WRITABLE) == 0u) {
        return EXT_STORE_ERR_RO;
    }
    /* 多步序列 + 共享帧缓冲：全程持锁（递归互斥，内部 MarkBad 可重入） */
    if (!ext_lock()) {
        return EXT_STORE_ERR_BUSY;
    }
    rc = ext_writesafe_locked(id, phys, stride, data, len);
    ext_unlock();
    return rc;
}

/* WriteSafe 锁内主体（调用方已持递归互斥） */
static int ext_writesafe_locked(ext_part_id_t id, uint32_t phys,
                                uint32_t stride, const void *data, uint32_t len)
{
    /* 构造帧（静态缓冲；大帧业务自行分块） */
    uint8_t *frame = s_safe_frame;
    if (len > 512u) {
        return EXT_STORE_ERR_PARAM;
    }
    ext_safe_frame_t *f = (ext_safe_frame_t *)frame;
    f->magic = EXT_SAFE_MAGIC;
    f->len = (uint16_t)len;
    f->ver = 1u;
    memcpy(f->data, data, len);
    /* CRC 覆盖 magic+len+ver+data（不含 crc 字段自身） */
    uint32_t crc = ext_crc32(frame, offsetof(ext_safe_frame_t, data) + len);
    memcpy(f->data + len, &crc, sizeof(crc));
    uint32_t total = EXT_SAFE_HEADER + len;

    const uint32_t base = s_parts[id].base + phys;
    /* 擦主副两槽 */
    for (int copy = 0; copy < 2; copy++) {
        uint32_t a = base + (uint32_t)copy * stride;
        if (ExtStore_IsBad(id, phys + (uint32_t)copy * stride)) {
            return EXT_STORE_ERR_BAD;
        }
        int erc = BSP_W25Q128_EraseSector(a);
        if (erc != BSP_W25Q_OK) {
            ExtStore_MarkBad(id, phys + (uint32_t)copy * stride);
            return EXT_STORE_ERR_IO;
        }
    }
    /* 先写副（B），再写主（A=提交点） */
    if (BSP_W25Q128_Write(base + stride, frame, total) != BSP_W25Q_OK) {
        return EXT_STORE_ERR_IO;
    }
    if (BSP_W25Q128_Write(base, frame, total) != BSP_W25Q_OK) {
        return EXT_STORE_ERR_IO;
    }
    /* 读回校验主帧 */
    uint8_t *check = s_safe_check;   /* 独立读回缓冲（避免覆盖 frame） */
    if (BSP_W25Q128_Read(base, check, total) != BSP_W25Q_OK ||
        memcmp(check, frame, total) != 0) {
        return EXT_STORE_ERR_IO;
    }
    return EXT_STORE_OK;
}

int ExtStore_ReadSafe(ext_part_id_t id, uint32_t slot, uint32_t stride,
                      void *buf, uint32_t len, uint32_t *out_ver)
{
    if ((stride % EXT_SECTOR) != 0u) {
        return EXT_STORE_ERR_PARAM;
    }
    uint32_t phys = slot * 2u * stride;
    int rc = ext_check_range(id, phys, 2u * stride);
    if (rc != EXT_STORE_OK) {
        return rc;
    }
    if (buf == NULL || len == 0u || len > 0xFFFEu) {
        return EXT_STORE_ERR_PARAM;
    }
    if (len > 512u) {
        return EXT_STORE_ERR_PARAM;
    }
    /* 共享帧缓冲：与 WriteSafe 互斥（避免读一半被覆盖） */
    if (!ext_lock()) {
        return EXT_STORE_ERR_BUSY;
    }
    uint8_t *frame = s_safe_frame;
    const uint32_t base = s_parts[id].base + phys;
    /* 主有效取主，否则取副 */
    for (int copy = 0; copy < 2; copy++) {
        uint32_t a = base + (uint32_t)copy * stride;
        if (BSP_W25Q128_Read(a, frame,
                             EXT_SAFE_HEADER + len + sizeof(uint32_t)) !=
            BSP_W25Q_OK) {
            continue;
        }
        ext_safe_frame_t *f = (ext_safe_frame_t *)frame;
        uint32_t crc = 0u;
        memcpy(&crc, f->data + len, sizeof(crc));
        if (f->magic == EXT_SAFE_MAGIC &&
            f->len == (uint16_t)len &&
            crc == ext_crc32(frame, offsetof(ext_safe_frame_t, data) + len)) {
            memcpy(buf, f->data, len);
            if (out_ver != NULL) {
                *out_ver = f->ver;
            }
            ext_unlock();
            return EXT_STORE_OK;
        }
    }
    ext_unlock();
    return EXT_STORE_ERR_CRC;
}
