/* ================================================================
 * TCP 服务：统一命令框架的 TCP 传输适配器（netconn API）
 *   - 端口 9000，行协议（CR/LF 结束），最多 2 个并发客户端
 *   - 行拆分/分发复用命令核心的 Cmd_SessionFeed（与未来 CAN 同一套）
 *   - 初始化时 Cmd_TransportRegister() 注册 "TCP" 传输（可插拔）
 *   - stream on：每秒推送关键遥测（uptime/heap/tasks/ETH）
 * ================================================================ */
#include "tcp_svc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "logger.h"
#include "eth_app.h"
#include "cmd_shell.h"

#define TCP_SVC_PORT          9000
#define TCP_SVC_BACKLOG       2
#define TCP_SVC_MAX_CLIENTS   2
#define TCP_SVC_TASK_STACK    2048
#define TCP_SVC_CLIENT_STACK  2048
#define TCP_SVC_IDLE_MS       120000
#define TCP_SVC_STREAM_MS     1000

/* TCP 传输适配器描述：注册到命令核心（传输名/掩码，命令零感知） */
static const cmd_transport_t s_tcp_transport = {
    .name  = "TCP",
    .mask  = CMD_TRANSPORT_TCP,
    .start = NULL,          /* 服务任务由 TcpSvc_Init 创建 */
};

/* TCP 客户端会话：统一命令框架 ctx->user 指向它 */
struct tcp_cli {
    struct netconn *conn;
    uint8_t         stream_on;
    uint8_t         peer_ip[4];
};

static tcp_svc_stat_t s_stat;
static osThreadId_t s_server_task = NULL;

const tcp_svc_stat_t *TcpSvc_GetStat(void)
{
    return &s_stat;
}

/* ---------------- 输出 ---------------- */

static void svc_write(struct netconn *conn, const char *s)
{
    if (conn == NULL || s == NULL || *s == '\0') {
        return;
    }
    netconn_write(conn, s, strlen(s), NETCONN_COPY);
}

static void svc_writef(struct netconn *conn, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svc_write(conn, buf);
}

/* 统一命令框架适配器输出：LOG_Printf 路由到当前 TCP 连接 */
static void tcp_ctx_out(cmd_ctx_t *ctx, const char *s, uint16_t len)
{
    struct tcp_cli *cli = (struct tcp_cli *)ctx->user;
    if (cli != NULL && cli->conn != NULL && s != NULL && len > 0) {
        netconn_write(cli->conn, s, len, NETCONN_COPY);
    }
}

/* ---------------- 客户端会话接口（供统一命令表） ---------------- */

int TcpSvc_ClientSetStream(tcp_cli_t *cli, uint8_t on)
{
    if (cli == NULL) {
        return -1;
    }
    cli->stream_on = on ? 1u : 0u;
    return 0;
}

uint8_t TcpSvc_ClientStream(tcp_cli_t *cli)
{
    return (cli != NULL) ? cli->stream_on : 0;
}

int TcpSvc_ClientPeerIP(tcp_cli_t *cli, uint8_t ip[4])
{
    if (cli == NULL || ip == NULL) {
        return -1;
    }
    memcpy(ip, cli->peer_ip, 4);
    return 0;
}

/* ---------------- 遥测流 ---------------- */

static void tcp_stream_tick(struct netconn *conn)
{
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t heap = (uint32_t)xPortGetFreeHeapSize();
    uint32_t tasks = (uint32_t)uxTaskGetNumberOfTasks();
    const eth_status_t *st = EthApp_GetStatus();
    svc_writef(conn,
               "UPTIME %lus HEAP %luB TASKS %lu ETH %s %u.%u.%u.%u\r\n",
               (unsigned long)(uptime / 1000u), (unsigned long)heap,
               (unsigned long)tasks,
               st->link_up ? "UP" : "DOWN",
               st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
}

/* ---------------- 客户端处理 ---------------- */

static void tcp_client_task(void *arg)
{
    struct netconn *conn = (struct netconn *)arg;
    struct tcp_cli cli;
    cmd_session_t sess;

    memset(&cli, 0, sizeof(cli));
    cli.conn = conn;

    ip_addr_t peer;
    u16_t peer_port = 0;
    if (netconn_peer(conn, &peer, &peer_port) == ERR_OK) {
        const ip4_addr_t *p4 = ip_2_ip4(&peer);
        cli.peer_ip[0] = ip4_addr1(p4);
        cli.peer_ip[1] = ip4_addr2(p4);
        cli.peer_ip[2] = ip4_addr3(p4);
        cli.peer_ip[3] = ip4_addr4(p4);
    }

    Cmd_SessionReset(&sess, CMD_TRANSPORT_TCP, &cli, tcp_ctx_out);

    svc_write(conn, "D00 Embedded Platform TCP Console\r\n");
    svc_write(conn, "Type 'help' for commands.\r\n" CMD_PROMPT);

    for (;;) {
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf(conn, &p);
        if (err == ERR_TIMEOUT) {
            if (cli.stream_on) {
                tcp_stream_tick(conn);
                svc_write(conn, CMD_PROMPT);
            }
            continue;
        }
        if (err != ERR_OK || p == NULL) {
            break;
        }

        u16_t off = 0;
        uint8_t chunk[64];
        while (off < p->tot_len) {
            u16_t take = (u16_t)((p->tot_len - off) < sizeof(chunk)
                                     ? (p->tot_len - off) : sizeof(chunk));
            pbuf_copy_partial(p, chunk, take, off);
            Cmd_SessionFeed(&sess, chunk, take);
            off += take;
        }
        pbuf_free(p);
        svc_write(conn, CMD_PROMPT);

        netconn_set_recvtimeout(conn, cli.stream_on ? TCP_SVC_STREAM_MS
                                                    : TCP_SVC_IDLE_MS);
    }

    s_stat.clients--;
    netconn_close(conn);
    netconn_delete(conn);
    vTaskDelete(NULL);
}

/* ---------------- 服务端 ---------------- */

static void tcp_server_task(void *arg)
{
    (void)arg;
    struct netconn *srv = netconn_new(NETCONN_TCP);
    if (srv == NULL) {
        LOG_Printf("TCP : server create failed\r\n");
        vTaskDelete(NULL);
        return;
    }
    netconn_set_recvtimeout(srv, 1000);
    if (netconn_bind(srv, IP_ADDR_ANY, TCP_SVC_PORT) != ERR_OK ||
        netconn_listen_with_backlog(srv, TCP_SVC_BACKLOG) != ERR_OK) {
        LOG_Printf("TCP : bind/listen failed on %u\r\n", TCP_SVC_PORT);
        netconn_delete(srv);
        vTaskDelete(NULL);
        return;
    }
    LOG_Printf("TCP : console listening on port %u\r\n", TCP_SVC_PORT);

    for (;;) {
        struct netconn *client = NULL;
        err_t err = netconn_accept(srv, &client);
        if (err != ERR_OK) {
            continue;
        }
        if (s_stat.clients >= TCP_SVC_MAX_CLIENTS) {
            s_stat.rejected++;
            netconn_close(client);
            netconn_delete(client);
            continue;
        }
        s_stat.accepted++;
        s_stat.clients++;

        osThreadAttr_t attr = {
            .name = "TcpCli",
            .stack_size = TCP_SVC_CLIENT_STACK,
            .priority = osPriorityNormal,
        };
        osThreadId_t h = osThreadNew(tcp_client_task, client, &attr);
        if (h == NULL) {
            s_stat.clients--;
            netconn_close(client);
            netconn_delete(client);
        }
    }
}

void TcpSvc_Init(void)
{
    memset(&s_stat, 0, sizeof(s_stat));
    Cmd_TransportRegister(&s_tcp_transport);
    osThreadAttr_t attr = {
        .name = "TcpSvc",
        .stack_size = TCP_SVC_TASK_STACK,
        .priority = osPriorityNormal,
    };
    s_server_task = osThreadNew(tcp_server_task, NULL, &attr);
    (void)s_server_task;
}
