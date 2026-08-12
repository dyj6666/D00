/* ================================================================
 * 以太网 TCP OTA 传输服务实现
 * ================================================================ */
#include "ota_tcp_svc.h"
#include "ota_agent.h"
#include "ota_transport.h"
#include "app_config.h"
#include "logger.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/tcp.h"

#include <string.h>

#define OTA_TCP_MAGIC      0x5Au
#define OTA_TCP_ACK        0x80u
#define OTA_TCP_HDR        4u          /* magic + cmd + len(2) */
/* DATA 帧 = 4 头 + 4 偏移 + 240 数据 + 1 CRC */
#define OTA_TCP_MAX_FRAME  (OTA_TCP_HDR + OTA_CHUNK_MAX + 4u + 1u)

#define OTA_TCP_CMD_BEGIN  0x01u
#define OTA_TCP_CMD_DATA   0x02u
#define OTA_TCP_CMD_END    0x03u
#define OTA_TCP_CMD_STATUS 0x04u
#define OTA_TCP_CMD_RESET  0x05u

static volatile uint32_t s_sessions = 0;

/* 支持 seed 的 CRC-8，便于对 cmd+len+payload 连续计算（与对端整段校验一致） */
static uint8_t ota_crc8_seed(uint8_t crc, const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t ota_crc8(const uint8_t *d, uint16_t len)
{
    return ota_crc8_seed(0, d, len);
}

static void tcp_send(struct netconn *conn, uint8_t cmd,
                     const uint8_t *payload, uint16_t len)
{
    /* 整帧一次性写入：多次小写会触发 Nagle/延迟 ACK 交互，
     * 实测每个 ACK 被拖慢 ~40ms（:9000 单次写入仅 1.7ms） */
    uint8_t frame[OTA_TCP_MAX_FRAME];
    frame[0] = OTA_TCP_MAGIC;
    frame[1] = cmd;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)(len & 0xFF);
    if (len > 0u && payload != NULL) {
        memcpy(frame + OTA_TCP_HDR, payload, len);
    }
    frame[OTA_TCP_HDR + len] = ota_crc8_seed(
        ota_crc8_seed(0, frame + 1, 3u), payload, len);
    (void)netconn_write(conn, frame, (u16_t)(OTA_TCP_HDR + len + 1u),
                        NETCONN_COPY);
}

static void tcp_ack1(struct netconn *conn, uint8_t status)
{
    tcp_send(conn, OTA_TCP_ACK, &status, 1u);
}

static void tcp_ack_rxtotal(struct netconn *conn, uint8_t status)
{
    uint8_t pl[9];
    uint8_t state;
    uint32_t rx = 0, total = 0;
    Ota_Status(&state, &rx, &total);
    pl[0] = status;
    pl[1] = (uint8_t)(rx >> 24); pl[2] = (uint8_t)(rx >> 16);
    pl[3] = (uint8_t)(rx >> 8);  pl[4] = (uint8_t)(rx & 0xFF);
    pl[5] = (uint8_t)(total >> 24); pl[6] = (uint8_t)(total >> 16);
    pl[7] = (uint8_t)(total >> 8);  pl[8] = (uint8_t)(total & 0xFF);
    tcp_send(conn, OTA_TCP_ACK, pl, sizeof(pl));
}

/* 处理单个命令帧；返回 0=继续 1=断开 */
static int tcp_handle_frame(struct netconn *conn, uint8_t cmd,
                            const uint8_t *pl, uint16_t len)
{
    switch (cmd) {
        case OTA_TCP_CMD_BEGIN: {
            if (len < 8u) {
                tcp_ack1(conn, 0xFE);
                break;
            }
            uint32_t ver = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                           ((uint32_t)pl[2] << 8) | pl[3];
            uint32_t size = ((uint32_t)pl[4] << 24) | ((uint32_t)pl[5] << 16) |
                            ((uint32_t)pl[6] << 8) | pl[7];
            uint8_t st = Ota_Begin(ver, size);
            tcp_ack1(conn, st);
            if (st == 0u) {
                s_sessions++;          /* 仅统计成功建立的会话 */
            }
            break;
        }
        case OTA_TCP_CMD_DATA: {
            if (len < 4u) {
                tcp_ack1(conn, 0xFE);
                break;
            }
            uint32_t off = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                           ((uint32_t)pl[2] << 8) | pl[3];
            uint8_t st = Ota_Data(off, pl + 4, (uint16_t)(len - 4u));
            tcp_ack_rxtotal(conn, st);
            break;
        }
        case OTA_TCP_CMD_END:
            tcp_ack1(conn, Ota_End());
            break;                       /* END 触发复位，连接随之断开 */
        case OTA_TCP_CMD_STATUS:
            {
                uint8_t st;
                uint32_t rx, total;
                Ota_Status(&st, &rx, &total);
                tcp_ack_rxtotal(conn, st);   /* 上报真实 OTA 状态，供客户端续传判断 */
            }
            break;
        case OTA_TCP_CMD_RESET:
            tcp_ack1(conn, Ota_Reset());
            break;
        default:
            tcp_ack1(conn, 0xFF);
            break;
    }
    return 0;
}

static void ota_tcp_session(struct netconn *conn)
{
    uint8_t buf[OTA_TCP_MAX_FRAME];
    uint16_t have = 0;
    for (;;) {
        err_t e;
        struct netbuf *nb = NULL;
        e = netconn_recv(conn, &nb);
        if (e != ERR_OK || nb == NULL) {
            break;                                     /* 超时/断开 */
        }
        /* 遍历 pbuf 链，避免大块数据跨 pbuf 边界时只读到首段 */
        netbuf_first(nb);
        do {
            void *d = NULL;
            u16_t l = 0;
            netbuf_data(nb, &d, &l);
            if (d != NULL && l > 0u) {
                const uint8_t *p = (const uint8_t *)d;
                u16_t off = 0;
                /* 流式逐帧：当前段灌入缓冲，凑够一帧立即处理。
                 * 若一次性只拷贝 249B 而丢弃段内剩余帧，流水线
                 * 多帧同段到达时会导致帧错位（实测 DATA 状态 2）。 */
                while (off < l) {
                    uint16_t room = (uint16_t)(sizeof(buf) - have);
                    uint16_t take = ((l - off) < room) ? (u16_t)(l - off)
                                                       : room;
                    memcpy(buf + have, p + off, take);
                    have = (uint16_t)(have + take);
                    off = (u16_t)(off + take);

                    /* 处理缓冲内已凑齐的完整帧 */
                    while (have >= OTA_TCP_HDR + 1u) {
                        if (buf[0] != OTA_TCP_MAGIC) {
                            have = 0;                  /* 失步：整帧重同步 */
                            break;
                        }
                        uint16_t plen = (uint16_t)((uint16_t)buf[2] << 8 |
                                                   buf[3]);
                        uint16_t total = (uint16_t)(OTA_TCP_HDR + plen + 1u);
                        if (total > sizeof(buf)) {
                            have = 0;                  /* 超长帧：防呆 */
                            break;
                        }
                        if (have < total) {
                            break;                     /* 等待完整帧 */
                        }
                        uint8_t crc = ota_crc8(buf + 1,
                                               (uint16_t)(3u + plen));
                        if (crc == buf[total - 1u]) {
                            if (tcp_handle_frame(conn, buf[1],
                                                 buf + OTA_TCP_HDR, plen)) {
                                have = 0;
                                netbuf_delete(nb);
                                return;
                            }
                        } else {
                            tcp_ack1(conn, 0xFF);      /* CRC 错 */
                        }
                        have = (uint16_t)(have - total);
                        if (have > 0u) {
                            memmove(buf, buf + total, have);
                        }
                    }
                    if (have == sizeof(buf)) {
                        have = 0;                      /* 缓冲被灌满仍无完整帧 */
                        break;
                    }
                }
            }
        } while (netbuf_next(nb) >= 0);
        netbuf_delete(nb);
    }
}

static void ota_tcp_task(void *arg)
{
    (void)arg;
    struct netconn *srv = netconn_new(NETCONN_TCP);
    if (srv == NULL) {
        LOG_Printf("[OTA-TCP] server alloc failed\r\n");
        vTaskDelete(NULL);
        return;
    }
    if (netconn_bind(srv, IP_ADDR_ANY, OTA_TCP_PORT) != ERR_OK) {
        LOG_Printf("[OTA-TCP] bind :%u FAILED\r\n", (unsigned)OTA_TCP_PORT);
        netconn_delete(srv);
        vTaskDelete(NULL);
        return;
    }
    netconn_set_recvtimeout(srv, 500);
    if (netconn_listen(srv) != ERR_OK) {
        LOG_Printf("[OTA-TCP] listen FAILED\r\n");
        netconn_delete(srv);
        vTaskDelete(NULL);
        return;
    }
    LOG_Printf("[OTA-TCP] server listening :%u (ETH OTA)\r\n",
               (unsigned)OTA_TCP_PORT);
    for (;;) {
        struct netconn *cli = NULL;
        if (netconn_accept(srv, &cli) == ERR_OK && cli != NULL) {
            LOG_Printf("[OTA-TCP] client connected\r\n");
            /* 关闭 Nagle：ACK 帧立即发送，避免流水线突发时小段 ACK 在
             * 发送队列堆积（TCP_SND_QUEUELEN 满）导致 netconn_write
             * 永久阻塞、OTA 服务挂死（实测 window>=16 复现）。 */
            tcp_nagle_disable(cli->pcb.tcp);
            netconn_set_recvtimeout(cli, 10000);
            ota_tcp_session(cli);
            netconn_close(cli);
            netconn_delete(cli);
            LOG_Printf("[OTA-TCP] client disconnected\r\n");
        } else if (cli != NULL) {
            netconn_delete(cli);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

uint32_t OtaTcpSvc_GetSessions(void)
{
    return s_sessions;
}

void OtaTcpSvc_Init(void)
{
    osThreadAttr_t attr = {
        .name = "OtaTcpSvc",
        .stack_size = 2048,   /* GCC 下 Ota_Begin(擦除+会话保存)+netconn 路径
                               * 栈深超过 Keil 实测 504B；1024B 导致 BEGIN 处理
                               * 后连接异常断开（实测断点确认进入 Ota_Begin 后
                               * 任务栈不足） */
        /* 与 TCP 控制台/HTTP 服务同优先级，避免低优先级导致
         * 每请求唤醒延迟 ~40ms（实测 :9000 Normal=1.6ms vs :9020 Below=43ms） */
        .priority = osPriorityNormal,
    };
    osThreadNew(ota_tcp_task, NULL, &attr);
}
