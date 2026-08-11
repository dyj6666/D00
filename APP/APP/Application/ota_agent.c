/* ================================================================
 * ota_agent —— 运行时 OTA：下载到 DOWNLOAD 区 + 启动确认
 *
 * 架构位置：APP 应用层；供 data_link / cmd_shell / OtaTcp / OtaHttp 调用
 * 核心流程：BEGIN -> DATA(240B/块) -> END -> 复位进 BOOT -> 启动确认成功
 * 关键约束：Flash 编程期间关中断；调用方必须顺序写、不跳块；
 *           安全校验（签名/解密/防回滚）由 BOOT 统一完成
 * ================================================================ */
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
#include "FreeRTOS.h"
#include "semphr.h"
#include "buzzer_app.h"

/* ---------------- 参数区（与 BOOT/boot_param.c 结构一致） ---------------- */
#pragma pack(1)
typedef struct {
    uint32_t magic;            /* OTA_PARAM_MAGIC，识别有效参数块 */
    uint32_t boot_state;       /* NORMAL/PENDING/RECOVERY/UPGRADE 状态机 */
    uint32_t boot_count;       /* 新固件启动尝试次数（超限回滚） */
    uint32_t rollback_count;   /* 历史回滚次数（诊断用） */
    uint32_t last_error;       /* 最近一次 BOOT 错误码 */
    uint32_t last_build_no;    /* 已接受的最大构建号（防重放） */
    uint32_t crc32;            /* 覆盖 magic..last_build_no 的校验和 */
} ota_param_t;
#pragma pack()

/* ---------------- 断点续传会话（DOWNLOAD 区尾部槽区） ---------------- */
#pragma pack(1)
typedef struct {
    uint32_t magic;       /* OTA_SESSION_MAGIC */
    uint32_t version;     /* 会话固件版本，续传须匹配 */
    uint32_t total;       /* 固件总长 */
    uint32_t received;    /* 已收字节，即续传起点 */
    uint32_t crc32;       /* 覆盖 magic..received */
} ota_session_t;          /* 20B，槽间距 32B */
#pragma pack()

static bool ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* ---------------- 内部状态 ---------------- */
static volatile uint8_t  ota_state = OTA_ST_IDLE;
static uint32_t ota_total = 0;         /* 固件总大小 */
static uint32_t ota_received = 0;      /* 已收字节 */
static uint32_t ota_begin_version = 0; /* 本次会话版本（会话槽持久化用） */
static SemaphoreHandle_t s_ota_mutex = NULL;  /* 传输互斥：防 UART/TCP/HTTP 并发操作 */

/* 并发保护：任何传输（UART CmdTask / TCP 任务 / HTTP shell）调用 Ota_*
 * 前必须取得互斥锁，防止两个传输同时通过状态检查导致下载区竞争。 */
static void ota_mutex_take(void)
{
    if (s_ota_mutex != NULL) {
        (void)xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(200u));
    }
}

static void ota_mutex_give(void)
{
    if (s_ota_mutex != NULL) {
        (void)xSemaphoreGive(s_ota_mutex);
    }
}

/** @brief 计算会话槽 CRC32（只覆盖 crc32 字段之前的数据） */
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

/**
 * @brief  将当前进度持久化到指定会话槽（每块写入一次，断电可续传）
 * @param  slot      槽号（0..OTA_SESSION_SLOTS-1）
 * @param  version   固件版本
 * @param  total     固件总长
 * @param  received  已收字节
 */
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

/** @brief 扫描槽区，返回最新（槽号最大）的有效会话；无部分会话返回 false */
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

/** @brief 失效全部会话槽：魔数写 0（1→0 无需擦除），约几十毫秒 */
static void ota_session_clear(void)
{
    uint8_t zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < OTA_SESSION_SLOTS; i++) {
        (void)ota_flash_write(OTA_SESSION_BASE + i * 32, zero, sizeof(zero));
    }
}

/** @brief 响应 shell "ota" 命令：写 BKP 升级标志并复位，触发 BOOT 升级模式 */
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
/** @brief 把 Flash 绝对地址映射为扇区号（覆盖 0x0800_0000~0x080F_FFFF） */
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

/**
 * @brief  擦除 [addr, addr+len) 覆盖的全部扇区
 * @param  addr  起始地址
 * @param  len   长度；0 表示仅擦 addr 所在扇区
 * @return true=全部扇区擦除成功
 * @note   DOWNLOAD 区 256KB（扇区 9+10）必须整区擦除后再写，
 *         否则编程 0→1 会失败
 */
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

/**
 * @brief  向 Flash 写入任意长度数据（自动处理字对齐）
 * @param  addr  目标地址（任意字节对齐）
 * @param  data  源数据指针
 * @param  len   字节数
 * @return true=全部写入成功
 * @note   整段编程期间关中断，保证原子性
 */
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

/** @brief 计算参数块 CRC32（只覆盖 crc32 字段之前的数据） */
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

/** @brief 启动确认：BOOT 置 PENDING 后，新固件首次正常运行即写 NORMAL 防回滚 */
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
        Buzzer_OtaSuccess();   /* 新固件确认成功：播"三短一长"完成旋律 */
    } else {
        LOG_Printf("OTA: confirm write FAILED\r\n");
    }
}

/* ---------------- 传输层命令入口（UART/TCP/HTTP 共用） ---------------- */
/**
 * @brief  开始一次 OTA 下载会话
 * @param  version  固件版本号（低于当前版本会被拒绝）
 * @param  size     固件包总长
 * @return 0=成功；1=已在接收中；2=长度非法；3=擦除失败；4=版本降级拒绝
 * @note   存在同版本同大小的部分会话时自动续传（不擦下载区）
 */
uint8_t Ota_Begin(uint32_t version, uint32_t size)
{
    ota_mutex_take();
    if (ota_state == OTA_ST_RECEIVING) {
        ota_mutex_give();
        Buzzer_OtaFail();
        return 1;   /* 已在接收中 */
    }
    if (size == 0 || size > OTA_DOWNLOAD_SAFE) {
        LOG_Printf("OTA: bad size %lu\r\n", (unsigned long)size);
        ota_mutex_give();
        Buzzer_OtaFail();
        return 2;
    }
    /* 版本降级拦截（BOOT 侧还会二次校验） */
    uint32_t cur = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    if (cur != 0xFFFFFFFFu && cur != 0u && version < cur) {
        LOG_Printf("OTA: version downgrade denied (%lu < %lu)\r\n",
                   (unsigned long)version, (unsigned long)cur);
        ota_mutex_give();
        Buzzer_OtaFail();
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
        ota_mutex_give();
        Buzzer_OtaStart();
        return 0;
    }

    LOG_Printf("OTA: erasing download area...\r\n");
    if (!ota_flash_erase(OTA_DOWNLOAD_ADDR, OTA_DOWNLOAD_SIZE)) {
        LOG_Printf("OTA: download erase FAILED\r\n");
        ota_mutex_give();
        Buzzer_OtaFail();
        return 3;
    }
    ota_total = size;
    ota_received = 0;
    ota_state = OTA_ST_RECEIVING;
    ota_session_save(0, version, size, 0);
    LOG_Printf("OTA: download area ready, awaiting data\r\n");
    ota_mutex_give();
    Buzzer_OtaStart();
    return 0;
}

/**
 * @brief  写入一块固件数据（严格顺序写）
 * @param  offset  本块相对固件起点的偏移，必须等于已收字节数
 * @param  data    块数据指针
 * @param  len     块长度，不得超过 OTA_CHUNK_MAX
 * @return 0=成功；1=非接收态；2=参数非法；3=Flash 写失败
 * @note   每块写入后持久化进度到会话槽，失败则状态回到 IDLE
 */
uint8_t Ota_Data(uint32_t offset, const uint8_t *data, uint16_t len)
{
    ota_mutex_take();
    if (ota_state != OTA_ST_RECEIVING) {
        ota_mutex_give();
        return 1;
    }
    if (len > OTA_CHUNK_MAX || offset != ota_received ||
        offset + len > ota_total || offset + len > OTA_DOWNLOAD_SAFE) {
        LOG_Printf("OTA: bad chunk off=%lu len=%u\r\n",
                   (unsigned long)offset, (unsigned)len);
        ota_mutex_give();
        return 2;
    }
    if (!ota_flash_write(OTA_DOWNLOAD_ADDR + offset, data, len)) {
        uint32_t probe = *(volatile uint32_t *)(OTA_DOWNLOAD_ADDR + offset);
        LOG_Printf("OTA: flash write FAILED at %lu probe=0x%08X SR=0x%08X\r\n",
                   (unsigned long)offset, (unsigned)probe,
                   (unsigned)(FLASH->SR));
        ota_state = OTA_ST_IDLE;
        ota_mutex_give();
        Buzzer_OtaFail();
        return 3;
    }
    ota_received += len;
    /* 每 16 块（3840B）持久化一次进度：每块省 1 次 Flash 写（约 1ms），
     * 三通道（UART/TCP/HTTP）整体提速；断点粒度 3840B，可接受。
     * 恢复点 = 实际已写位置，避免重写已写区域导致 Flash 编程失败。 */
    if ((ota_received % (OTA_CHUNK_MAX * 16u)) == 0u) {
        uint32_t slot = ota_received / OTA_CHUNK_MAX;
        if (slot < OTA_SESSION_SLOTS) {
            ota_session_save(slot, ota_begin_version, ota_total, ota_received);
        }
    }
    ota_mutex_give();
    return 0;
}

/**
 * @brief  结束下载：校验收齐后写升级标志并复位进 BOOT
 * @return 0=成功（随后复位，不会返回）；1=非接收态；2=固件不完整
 * @note   采用参数区 UPGRADE 状态 + BKP 标志双保险触发
 */
uint8_t Ota_End(void)
{
    ota_mutex_take();
    if (ota_state != OTA_ST_RECEIVING) {
        ota_mutex_give();
        return 1;
    }
    if (ota_received != ota_total) {
        LOG_Printf("OTA: incomplete %lu/%lu\r\n",
                   (unsigned long)ota_received, (unsigned long)ota_total);
        ota_state = OTA_ST_IDLE;
        ota_mutex_give();
        Buzzer_OtaFail();
        return 2;
    }
    ota_state = OTA_ST_DONE;
    LOG_Printf("OTA: download complete (%lu B), rebooting to BOOT...\r\n",
               (unsigned long)ota_total);
    ota_session_save(OTA_SESSION_SLOTS - 1, ota_begin_version,
                     ota_total, ota_total);
    Buzzer_OtaDownloadDone();   /* 下载完成：滴-滴 收尾音（阻塞式，随后触发切换） */

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
    ota_mutex_give();
    BSP_SystemReset();
    return 0;   /* 不会到达 */
}

/** @brief 读取当前 OTA 状态与进度（供 status 命令/续传客户端使用） */
uint8_t Ota_Status(uint8_t *state, uint32_t *received, uint32_t *total)
{
    ota_mutex_take();
    *state = ota_state;
    *received = ota_received;
    *total = ota_total;
    ota_mutex_give();
    return 0;
}

/** @brief 强制回到 IDLE 并清空全部会话槽（配合 --no-resume 从零开始） */
uint8_t Ota_Reset(void)
{
    ota_mutex_take();
    ota_state = OTA_ST_IDLE;
    ota_received = 0;
    ota_total = 0;
    ota_session_clear();
    LOG_Printf("OTA: session reset (fresh download required)\r\n");
    ota_mutex_give();
    return 0;
}

/** @brief 危险自测：把参数区置为 PENDING+MAX，下次复位触发 BOOT 回滚 */
void Ota_ForceRollbackTest(void)
{
    ota_mutex_take();
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
    ota_mutex_give();
    BSP_SystemReset();
}

/** @brief OTA Agent 初始化：订阅命令、执行启动确认、打印参数区状态 */
void OtaAgent_Init(void)
{
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
    }
    EventBus_Subscribe(MSG_CMD_OTA_START, handle_ota_start_msg);
    ota_confirm_startup();
    ota_param_t st;
    memcpy(&st, (const void *)OTA_PARAM_ADDR, sizeof(st));
    if (st.magic == OTA_PARAM_MAGIC && st.crc32 == ota_param_crc(&st)) {
        LOG_Printf("[APP] OTA  : Agent ready (last build %lu)\r\n",
                   (unsigned long)st.last_build_no);
        if (st.boot_state == OTA_STATE_RECOVERY) {
            Buzzer_OtaFail();   /* 回滚超限进入恢复模式：三短音提示人工介入 */
        }
    } else {
        LOG_Printf("[APP] OTA  : Agent ready (param invalid)\r\n");
    }
}
