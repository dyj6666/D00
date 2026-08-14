/* ================================================================
 * ota_agent —— 运行时 OTA：下载到外部 Flash ota_dl 槽 + 启动确认
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
#include "FreeRTOS.h"
#include "semphr.h"
#include "buzzer_app.h"
#include "bsp_flash.h"
#include "ext_store.h"
#include "data_link.h"
#include "protocol.h"

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
    bool ok = BSP_Flash_Write(OTA_SESSION_BASE + slot * 32,
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
    uint32_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};   /* 32B = 槽间距 */
    for (uint32_t i = 0; i < OTA_SESSION_SLOTS; i++) {
        /* 已擦除（全 0xFF）的槽无需写：Flash 写 ~50µs/字，正常会话只写少量槽，
         * 跳过可把整区清除从 ~300ms 降到微秒级（Ota_Reset 可安全在事件总线执行） */
        const uint32_t *p =
            (const uint32_t *)(OTA_SESSION_BASE + i * 32u);
        if (p[0] == 0xFFFFFFFFu && p[1] == 0xFFFFFFFFu &&
            p[2] == 0xFFFFFFFFu && p[3] == 0xFFFFFFFFu &&
            p[4] == 0xFFFFFFFFu && p[5] == 0xFFFFFFFFu &&
            p[6] == 0xFFFFFFFFu && p[7] == 0xFFFFFFFFu) {
            continue;
        }
        (void)BSP_Flash_Write(OTA_SESSION_BASE + i * 32u,
                              (const uint8_t *)zero, sizeof(zero));
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
    bool e = BSP_Flash_EraseRange(OTA_PARAM_ADDR, 0);
    bool w0 = BSP_Flash_Write(OTA_PARAM_ADDR, (const uint8_t *)&p, sizeof(p));
    bool w1 = BSP_Flash_Write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
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
    if (size == 0 || size > OTA_EXT_DL_SAFE) {
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
        BSP_Flash_ResetController();
        ota_total = size;
        ota_received = sess.received;
        ota_state = OTA_ST_RECEIVING;
        ota_mutex_give();
        Buzzer_OtaStart();
        return 0;
    }

    LOG_Printf("OTA: erasing download area...\r\n");
    if (ExtStore_EraseRange(EXT_PART_OTA_DL, 0u, size) != EXT_STORE_OK) {
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
        offset + len > ota_total || offset + len > OTA_EXT_DL_SAFE) {
        LOG_Printf("OTA: bad chunk off=%lu len=%u\r\n",
                   (unsigned long)offset, (unsigned)len);
        ota_mutex_give();
        return 2;
    }
    if (ExtStore_Write(EXT_PART_OTA_DL, offset, data, len) != EXT_STORE_OK) {
        uint32_t probe = 0u;
        (void)ExtStore_Read(EXT_PART_OTA_DL, offset, &probe, sizeof(probe));
        LOG_Printf("OTA: flash write FAILED at %lu probe=0x%08X SR=0x%08X\r\n",
                   (unsigned long)offset, (unsigned)probe,
                   (unsigned)0u);
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
    BSP_Flash_EraseRange(OTA_PARAM_ADDR, 0);
    BSP_Flash_Write(OTA_PARAM_ADDR, (const uint8_t *)&param, sizeof(param));
    BSP_Flash_Write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
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
    BSP_Flash_EraseRange(OTA_PARAM_ADDR, 0);
    BSP_Flash_Write(OTA_PARAM_ADDR, (const uint8_t *)&param, sizeof(param));
    BSP_Flash_Write(OTA_PARAM_ADDR + OTA_PARAM_SLOT_OFFSET,
                    (const uint8_t *)&param, sizeof(param));
    LOG_Printf("OTA: rollback test armed (PENDING+MAX), resetting...\r\n");
    BSP_DelayMs(100);
    ota_mutex_give();
    BSP_SystemReset();
}

/** @brief OTA Agent 初始化：订阅命令、执行启动确认、打印参数区状态 */
/* HOSTLINK 数据链路的 OTA 命令处理（BEGIN/DATA/END/STATUS/RESET）。
 * 由 DataLink_SetOtaHandler 注册，data_link 层只透传帧，不感知 OTA 细节。 */
static void data_link_ota_handler(uint8_t cmd, const uint8_t *payload,
                                  uint8_t payload_len)
{
    switch (cmd) {
        case CMD_OTA_BEGIN: {
            /* 请求：version(u32 LE) + size(u32 LE) */
            if (payload_len != 8u) {
                DataLink_SendError(cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint32_t ver = (uint32_t)(payload[0] | (payload[1] << 8) |
                                      (payload[2] << 16) | (payload[3] << 24));
            uint32_t size = (uint32_t)(payload[4] | (payload[5] << 8) |
                                       (payload[6] << 16) | (payload[7] << 24));
            uint8_t st = Ota_Begin(ver, size);
            DataLink_SendFrame(cmd, &st, 1);
            break;
        }
        case CMD_OTA_DATA: {
            /* 请求：offset(u32 LE) + data(<=120B) */
            if (payload_len < 5u || payload_len > 4u + OTA_CHUNK_MAX) {
                DataLink_SendError(cmd, PROTO_ERR_BAD_PAYLOAD_LEN);
                break;
            }
            uint32_t off = (uint32_t)(payload[0] | (payload[1] << 8) |
                                      (payload[2] << 16) | (payload[3] << 24));
            uint8_t st = Ota_Data(off, &payload[4],
                                  (uint16_t)(payload_len - 4u));
            uint32_t rx = 0, total = 0;
            uint8_t state = 0;
            Ota_Status(&state, &rx, &total);
            uint8_t resp[5] = { st, 0, 0, 0, 0 };
            resp[1] = (uint8_t)(rx & 0xFF);
            resp[2] = (uint8_t)((rx >> 8) & 0xFF);
            resp[3] = (uint8_t)((rx >> 16) & 0xFF);
            resp[4] = (uint8_t)((rx >> 24) & 0xFF);
            DataLink_SendFrame(cmd, resp, sizeof(resp));
            break;
        }
        case CMD_OTA_END: {
            uint8_t st = Ota_End();
            DataLink_SendFrame(cmd, &st, 1);
            break;
        }
        case CMD_OTA_STATUS: {
            uint8_t state = 0;
            uint32_t rx = 0, total = 0;
            Ota_Status(&state, &rx, &total);
            uint8_t resp[9] = { state, 0, 0, 0, 0, 0, 0, 0, 0 };
            resp[1] = (uint8_t)(rx & 0xFF);
            resp[2] = (uint8_t)((rx >> 8) & 0xFF);
            resp[3] = (uint8_t)((rx >> 16) & 0xFF);
            resp[4] = (uint8_t)((rx >> 24) & 0xFF);
            resp[5] = (uint8_t)(total & 0xFF);
            resp[6] = (uint8_t)((total >> 8) & 0xFF);
            resp[7] = (uint8_t)((total >> 16) & 0xFF);
            resp[8] = (uint8_t)((total >> 24) & 0xFF);
            DataLink_SendFrame(cmd, resp, sizeof(resp));
            break;
        }
        case CMD_OTA_RESET: {
            uint8_t st = Ota_Reset();
            DataLink_SendFrame(cmd, &st, 1);
            break;
        }
        default:
            DataLink_SendError(cmd, PROTO_ERR_UNKNOWN_CMD);
            break;
    }
}

void OtaAgent_Init(void)
{
    DataLink_SetOtaHandler(data_link_ota_handler);
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
