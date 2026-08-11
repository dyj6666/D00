/* ================================================================
 * 以太网 HTTP OTA 传输服务（客户端拉取模型）
 *   - `ota http <ip[:port]>/<path>`：GET 下载加密签名固件包
 *   - 解析响应头 Content-Length 得总长，读取包首部 12B 解析版本号，
 *     然后 Ota_Begin -> 流式 Ota_Data -> Ota_End（与 UART/TCP 共用下载核心）
 *   - 阻塞执行（shell 上下文），进度打日志；安全校验由 BOOT 完成
 *   - 兼容任意 netbuf/pbuf 边界：遍历 pbuf 链 + 头尾进位处理
 * ================================================================ */
#include "ota_http_svc.h"
#include "ota_agent.h"
#include "logger.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "app_config.h"
#include "buzzer_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_HTTP_HDR_BUF    512u

/* 包头部 32B（magic|version|len|iv|chip|build），版本号位于偏移 4..7；
 * 只需读前 12B 即可取出版本号，随后整包从偏移 0 顺序写入 */
#define OTA_PKG_HDR_LEN     12u
#define OTA_HTTP_PROGRESS   (OTA_CHUNK_MAX * 200u)  /* 每 48KB 打印一次进度：防刷屏 */

typedef struct {
    uint8_t  pre[OTA_PKG_HDR_LEN];   /* 包首部缓冲（取版本号） */
    uint16_t pre_len;
    uint8_t  started;                /* Ota_Begin 已调用 */
    uint32_t offset;                 /* 已写入下载区字节数 */
    uint32_t total;                  /* Content-Length（整包长度） */
    int      rc;                     /* 首个错误码，0=正常 */
} http_ota_ctx_t;

/* 头部行 key 比较（大小写不敏感），匹配 "key:" 返回 1 */
static int http_hdr_key_eq(const uint8_t *line, uint16_t len, const char *key)
{
    uint16_t k = 0;
    while (key[k] != '\0') {
        if (k >= len) return 0;
        char c = (char)line[k];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != key[k]) return 0;
        k++;
    }
    return (k < len && line[k] == ':');
}

/* 流式喂给 OTA 下载核心：先攒满包首部解析版本号 -> Ota_Begin -> 顺序 Ota_Data */
static int http_ota_feed(http_ota_ctx_t *c, const uint8_t *data, uint16_t len)
{
    while (len > 0u && c->rc == 0) {
        if (!c->started) {
            uint16_t need = (uint16_t)(OTA_PKG_HDR_LEN - c->pre_len);
            uint16_t take = (len < need) ? len : need;
            memcpy(c->pre + c->pre_len, data, take);
            c->pre_len = (uint16_t)(c->pre_len + take);
            data += take;
            len = (uint16_t)(len - take);
            if (c->pre_len < OTA_PKG_HDR_LEN) {
                break;                       /* 等待更多 body */
            }
            /* 包头部为小端（cryptor.py '<III12sII'），版本号位于偏移 4..7 */
            uint32_t ver = (uint32_t)c->pre[4] |
                           ((uint32_t)c->pre[5] << 8)  |
                           ((uint32_t)c->pre[6] << 16) |
                           ((uint32_t)c->pre[7] << 24);
            /* HTTP 拉取总是从 0 顺序写：先清残留会话，避免命中同版本断点续传
             * 导致 offset 不一致（TCP 服务器路径保留续传能力，走 STATUS 查询） */
            (void)Ota_Reset();
            uint8_t stt = Ota_Begin(ver, c->total);
            if (stt != 0u) {
                LOG_Printf("[OTA-HTTP] Ota_Begin failed %u\r\n", (unsigned)stt);
                c->rc = -9;
                break;
            }
            if (Ota_Data(0, c->pre, OTA_PKG_HDR_LEN) != 0u) {
                LOG_Printf("[OTA-HTTP] Ota_Data(pkg hdr) failed\r\n");
                c->rc = -10;
                break;
            }
            c->offset = OTA_PKG_HDR_LEN;
            c->started = 1;
            continue;
        }
        uint16_t chunk = (len > OTA_CHUNK_MAX) ? OTA_CHUNK_MAX : len;
        if (c->offset + chunk > c->total) {
            chunk = (uint16_t)(c->total - c->offset);
        }
        if (chunk == 0u) {
            break;
        }
        if (Ota_Data(c->offset, data, chunk) != 0u) {
            LOG_Printf("[OTA-HTTP] Ota_Data failed @%lu\r\n",
                       (unsigned long)c->offset);
            c->rc = -11;
            break;
        }
        c->offset += chunk;
        data += chunk;
        len = (uint16_t)(len - chunk);
        if ((c->offset % OTA_HTTP_PROGRESS) == 0u) {
            LOG_Printf("[OTA-HTTP] %lu/%lu\r\n",
                       (unsigned long)c->offset, (unsigned long)c->total);
        }
    }
    return c->rc;
}

int OtaHttp_Download(const char *host, uint16_t port, const char *path)
{
    if (host == NULL || path == NULL || port == 0u) {
        return -1;
    }

    struct netconn *conn = netconn_new(NETCONN_TCP);
    if (conn == NULL) {
        return -2;
    }
    netconn_set_recvtimeout(conn, 15000);

    ip_addr_t dst;
    if (!ip4addr_aton(host, ip_2_ip4(&dst))) {
        netconn_delete(conn);
        return -3;
    }
    if (netconn_connect(conn, &dst, port) != ERR_OK) {
        netconn_delete(conn);
        return -4;
    }

    char req[160];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, host);
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        netconn_delete(conn);
        return -5;
    }
    if (netconn_write(conn, req, (u16_t)rl, NETCONN_COPY) != ERR_OK) {
        netconn_delete(conn);
        return -6;
    }

    uint8_t  hdr[OTA_HTTP_HDR_BUF];
    uint16_t hdr_len = 0;
    uint8_t  hdr_done = 0;
    uint32_t content_len = 0;
    http_ota_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int ret = -7;
    int timeout_hits = 0;

    LOG_Printf("[OTA-HTTP] GET http://%s:%u%s\r\n", host, (unsigned)port, path);
    for (;;) {
        struct netbuf *nb = NULL;
        err_t e = netconn_recv(conn, &nb);
        if (e != ERR_OK || nb == NULL) {
            /* 中途收包超时（如偶发丢包+TCP 重传延迟）：连接仍可能存活，重试等待；
             * 连续 6 次仍无数据才判定失败，避免长时间阻塞 shell */
            if (e == ERR_TIMEOUT && ctx.started && ctx.offset < ctx.total &&
                ++timeout_hits <= 6) {
                LOG_Printf("[OTA-HTTP] recv timeout @%lu, retry %d/6...\r\n",
                           (unsigned long)ctx.offset, timeout_hits);
                continue;
            }
            LOG_Printf("[OTA-HTTP] recv end/err (%d) offset=%lu/%lu\r\n",
                       (int)e, (unsigned long)ctx.offset,
                       (unsigned long)ctx.total);
            ret = (ctx.started && ctx.offset >= ctx.total) ? 0 : -7;
            break;
        }
        timeout_hits = 0;
        /* 遍历 pbuf 链：大响应跨 pbuf 边界也能完整读取 */
        netbuf_first(nb);
        do {
            void *d = NULL;
            u16_t l = 0;
            netbuf_data(nb, &d, &l);
            if (d == NULL || l == 0u) {
                continue;
            }
            const uint8_t *p = (const uint8_t *)d;
            u16_t off = 0;
            if (!hdr_done) {
                while (off < l && !hdr_done && ret == -7) {
                    if (hdr_len >= sizeof(hdr)) {
                        LOG_Printf("[OTA-HTTP] header overflow\r\n");
                        ret = -8;
                        break;
                    }
                    hdr[hdr_len++] = p[off++];
                    if (hdr_len >= 4u &&
                        hdr[hdr_len - 4u] == '\r' &&
                        hdr[hdr_len - 3u] == '\n' &&
                        hdr[hdr_len - 2u] == '\r' &&
                        hdr[hdr_len - 1u] == '\n') {
                        /* 解析 Content-Length */
                        uint16_t ls = 0;
                        content_len = 0;
                        while (ls + 1u < hdr_len) {
                            uint16_t le = ls;
                            while (le < hdr_len && hdr[le] != '\n') le++;
                            if (http_hdr_key_eq(hdr + ls,
                                                (uint16_t)(le - ls),
                                                "content-length")) {
                                content_len = (uint32_t)strtoul(
                                    (const char *)(hdr + ls + 15), NULL, 10);
                                break;
                            }
                            ls = (uint16_t)(le + 1u);
                        }
                        if (content_len == 0u ||
                            content_len > OTA_DOWNLOAD_SAFE) {
                            LOG_Printf("[OTA-HTTP] bad content-length %lu\r\n",
                                       (unsigned long)content_len);
                            ret = -8;
                            break;
                        }
                        ctx.total = content_len;
                        hdr_done = 1;
                        hdr_len = 0;
                    }
                }
            }
            if (hdr_done && ret == -7 && off < l) {
                /* 头结束，本段剩余字节即 body（跨段由下一个 pbuf 段继续） */
                (void)http_ota_feed(&ctx, p + off, (uint16_t)(l - off));
            }
        } while (netbuf_next(nb) >= 0 && ret == -7);
        netbuf_delete(nb);
        if (ret != -7) {
            break;
        }
        if (hdr_done && ctx.started && ctx.offset >= ctx.total) {
            uint8_t stt = Ota_End();
            LOG_Printf("[OTA-HTTP] complete, Ota_End=%u\r\n", (unsigned)stt);
            ret = (stt == 0u) ? 0 : -12;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    netconn_delete(conn);
    if (ret != 0) {
        Buzzer_OtaFail();   /* 拉取失败：三短音警示（写失败已在 ota_agent 响应） */
    }
    return ret;
}
