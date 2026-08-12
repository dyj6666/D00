/* ================================================================
 * http_svc —— HTTP 状态服务（:8080，HTML/JSON 状态页）
 *
 * 架构位置：APP 应用层；独立 HttpSvc 任务，单连接串行处理
 * 核心流程：accept -> 解析 GET /api/status -> 组 JSON/HTML -> 响应
 * 关键约束：静态缓冲防任务栈溢出；单连接串行避免并发竞争
 * ================================================================ */
#include "http_svc.h"
#include "logger.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "eth_app.h"
#include "icmp_svc.h"
#include "usr_store.h"
#include "sntp_svc.h"
#include "mqtt_svc.h"

#include <stdio.h>
#include <string.h>

#define HTTP_PORT      8080u
#define HTTP_BUF       1024

static volatile uint8_t  s_enabled = 1;
static volatile uint32_t s_requests = 0;

uint32_t HttpSvc_GetRequests(void)
{
    return s_requests;
}

uint8_t HttpSvc_Enabled(void)
{
    return s_enabled ? 1u : 0u;
}

void HttpSvc_SetEnabled(uint8_t on)
{
    s_enabled = on ? 1u : 0u;
}

static void http_build_body(char *buf, uint32_t len, int json)
{
    EthApp_RefreshStatus();
    const eth_status_t *es = EthApp_GetStatus();
    const icmp_svc_stat_t *is = IcmpSvc_GetStat();
    const mqtt_svc_stat_t *ms = MqttSvc_GetStat();
    uint32_t used = 0, free = 0;
    UsrStore_Info(&used, &free);
    char ts[32];
    SntpSvc_GetTimeStr(ts, sizeof(ts));
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;

    if (json) {
        snprintf(buf, len,
                 "{\"ver\":%lu,\"uptime_s\":%lu,\"heap_free\":%lu,"
                 "\"link\":%u,\"ip\":\"%u.%u.%u.%u\","
                 "\"rx\":%lu,\"tx\":%lu,"
                 "\"icmp_rx\":%lu,\"icmp_tx\":%lu,"
                 "\"usr_keys\":%lu,\"usr_used\":%lu,\"usr_free\":%lu,"
                 "\"mqtt\":%u,\"rtc\":\"%s\"}",
                 (unsigned long)ver,
                 (unsigned long)(HAL_GetTick() / 1000u),
                 (unsigned long)xPortGetFreeHeapSize(),
                 (unsigned)es->link_up,
                 es->ip[0], es->ip[1], es->ip[2], es->ip[3],
                 (unsigned long)es->rx_packets,
                 (unsigned long)es->tx_packets,
                 (unsigned long)is->echo_rx,
                 (unsigned long)is->echo_tx,
                 (unsigned long)UsrStore_Count(),
                 (unsigned long)used, (unsigned long)free,
                 (unsigned)ms->state, ts);
    } else {
        snprintf(buf, len,
                 "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                 "<title>D00 Status</title></head><body>"
                 "<h1>D00 Embedded Platform</h1>"
                 "<p>ver %lu | uptime %lus | heap free %lu B</p>"
                 "<p>link %s | ip %u.%u.%u.%u | rx %lu tx %lu</p>"
                 "<p>icmp rx %lu tx %lu | usr keys %lu (%luB/%luB)</p>"
                 "<p>mqtt %u | rtc %s</p>"
                 "<p><a href=\"/api/status\">JSON</a></p>"
                 "</body></html>",
                 (unsigned long)ver,
                 (unsigned long)(HAL_GetTick() / 1000u),
                 (unsigned long)xPortGetFreeHeapSize(),
                 es->link_up ? "UP" : "DOWN",
                 es->ip[0], es->ip[1], es->ip[2], es->ip[3],
                 (unsigned long)es->rx_packets,
                 (unsigned long)es->tx_packets,
                 (unsigned long)is->echo_rx,
                 (unsigned long)is->echo_tx,
                 (unsigned long)UsrStore_Count(),
                 (unsigned long)used, (unsigned long)free,
                 (unsigned)ms->state, ts);
    }
}

static void http_handle(struct netconn *conn)
{
    /* 静态缓冲：单连接串行处理，避免大局部数组压爆服务任务栈 */
    static char buf[HTTP_BUF];
    static char body[HTTP_BUF];
    int n = 0;
    /* 读请求行（GET /path HTTP/1.x）；单连接串行处理 */
    struct netbuf *nb = NULL;
    if (netconn_recv(conn, &nb) == ERR_OK && nb != NULL) {
        void *d = NULL;
        u16_t l = 0;
        netbuf_data(nb, &d, &l);
        if (d != NULL) {
            int cl = (l < (u16_t)(HTTP_BUF - 1)) ? l : (HTTP_BUF - 1);
            memcpy(buf, d, cl);
            buf[cl] = '\0';
            if (cl >= 15 && strncmp(buf, "GET /api/status", 15) == 0) {
                n = 1;
            } else if (cl >= 5 && strncmp(buf, "GET /", 5) == 0) {
                n = 2;
            }
        }
        netbuf_delete(nb);
    } else {
        /* 连接无数据/超时：仍回 400 并关闭，避免客户端悬挂 */
        const char *bad = "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        netconn_write(conn, bad, (u16_t)strlen(bad), NETCONN_COPY);
        return;
    }

    const char *mime = "text/html";
    if (n == 1) {
        http_build_body(body, sizeof(body), 1);
        mime = "application/json";
    } else if (n == 2) {
        http_build_body(body, sizeof(body), 0);
    } else {
        snprintf(body, sizeof(body), "404 Not Found");
        mime = "text/plain";
    }

    char hdr[160];
    int bl = (int)strlen(body);
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 %s\r\nContent-Type: %s\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n",
             (n != 0) ? "200 OK" : "404 Not Found", mime, bl);
    netconn_write(conn, hdr, (u16_t)strlen(hdr), NETCONN_COPY);
    netconn_write(conn, body, (u16_t)bl, NETCONN_COPY);
    if (n != 0) {
        s_requests++;
    }
}

static void http_task(void *arg)
{
    (void)arg;
    struct netconn *srv = netconn_new(NETCONN_TCP);
    if (srv == NULL) {
        LOG_Printf("[HTTP] server alloc failed\r\n");
        vTaskDelete(NULL);
        return;
    }
    netconn_bind(srv, IP_ADDR_ANY, HTTP_PORT);
    netconn_set_recvtimeout(srv, 500);   /* accept 不无限阻塞，单请求不拖垮服务 */
    netconn_listen(srv);
    LOG_Printf("[HTTP] status server listening :%u\r\n", (unsigned)HTTP_PORT);
    for (;;) {
        struct netconn *cli = NULL;
        if (s_enabled && netconn_accept(srv, &cli) == ERR_OK && cli != NULL) {
            netconn_set_recvtimeout(cli, 1500);
            http_handle(cli);
            netconn_close(cli);
            netconn_delete(cli);
        } else if (cli != NULL) {
            netconn_delete(cli);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void HttpSvc_Init(void)
{
    osThreadAttr_t attr = {
        .name = "HttpSvc",
        .stack_size = 1024,   /* 峰值 ~544B（HW 376 词） */
        .priority = osPriorityBelowNormal,
    };
    osThreadNew(http_task, NULL, &attr);
}
