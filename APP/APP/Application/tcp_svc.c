/* ================================================================
 * TCP 服务实现：netconn 阻塞式服务器任务 + 每客户端处理任务
 * ================================================================ */
#include "tcp_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "main.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "logger.h"
#include "event_bus.h"
#include "data_link.h"
#include "eth_app.h"
#include "imu_svc.h"
#include "bsp_gpio.h"
#include "buzzer_app.h"
#include "err_mgr.h"

#define TCP_SVC_PORT          9000
#define TCP_SVC_BACKLOG       2
#define TCP_SVC_MAX_CLIENTS   2
#define TCP_SVC_TASK_STACK    2048
#define TCP_SVC_CLIENT_STACK  2048
#define TCP_SVC_MAX_LINE      160
#define TCP_SVC_IDLE_MS       120000
#define TCP_SVC_STREAM_MS     1000

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

/* ---------------- 遥测 ---------------- */

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

/* ---------------- 命令 ---------------- */

static void tcp_cmd_help(struct netconn *conn)
{
    svc_write(conn,
              "help       list commands\r\n"
              "info       system summary\r\n"
              "ver        firmware version\r\n"
              "sysmon     system monitor\r\n"
              "taskstats  task list & stack usage\r\n"
              "net        ethernet status\r\n"
              "led <on|off|toggle>\r\n"
              "beep <ms>\r\n"
              "mpu        IMU status\r\n"
              "echo <txt> connectivity test\r\n"
              "stream <on|off> periodic telemetry\r\n");
}

static void tcp_cmd_info(struct netconn *conn)
{
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    const eth_status_t *st = EthApp_GetStatus();
    svc_writef(conn,
               "Firmware v%lu | uptime %lus | heap %luB | tasks %u\r\n"
               "ETH: %s %u.%u.%u.%u | RX %lu TX %lu\r\n",
               (unsigned long)ver, (unsigned long)(uptime / 1000u),
               (unsigned long)xPortGetFreeHeapSize(),
               (unsigned)uxTaskGetNumberOfTasks(),
               st->link_up ? "UP" : "DOWN",
               st->ip[0], st->ip[1], st->ip[2], st->ip[3],
               (unsigned long)st->rx_packets,
               (unsigned long)st->tx_packets);
}

static void tcp_cmd_sysmon(struct netconn *conn)
{
    svc_writef(conn,
               "HEAP %luB | TASKS %u | EVT_LOST %lu | CMD_LOST %lu | TX_LOST %lu\r\n"
               "CRASH_SEQ %lu | IWDG active\r\n",
               (unsigned long)xPortGetFreeHeapSize(),
               (unsigned)uxTaskGetNumberOfTasks(),
               (unsigned long)EventBus_GetLostCount(),
               (unsigned long)DataLink_GetCmdLostCount(),
               (unsigned long)DataLink_GetTxLostCount(),
               (unsigned long)ERR_GetCrashSeq());
}

static void tcp_cmd_taskstats(struct netconn *conn)
{
    UBaseType_t size = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = pvPortMalloc(size * sizeof(TaskStatus_t));
    if (arr == NULL) {
        svc_write(conn, "(no memory)\r\n");
        return;
    }
    size = uxTaskGetSystemState(arr, size, NULL);
    svc_write(conn, "Task\tState\tPrio\tStack\t#\r\n");
    for (UBaseType_t i = 0; i < size; i++) {
        char st = 'X';
        switch (arr[i].eCurrentState) {
            case eRunning:   st = 'R'; break;
            case eBlocked:   st = 'B'; break;
            case eSuspended: st = 'S'; break;
            case eDeleted:   st = 'D'; break;
            default:         break;
        }
        svc_writef(conn, "%-12s\t%c\t%u\t%u\t%u\r\n",
                   arr[i].pcTaskName, st,
                   (unsigned)arr[i].uxCurrentPriority,
                   (unsigned)arr[i].usStackHighWaterMark,
                   (unsigned)arr[i].xTaskNumber);
    }
    vPortFree(arr);
}

static void tcp_cmd_net(struct netconn *conn)
{
    EthApp_RefreshStatus();
    const eth_status_t *st = EthApp_GetStatus();
    svc_writef(conn,
               "Link %s | IP %u.%u.%u.%u | GW %u.%u.%u.%u\r\n"
               "MAC %02X:%02X:%02X:%02X:%02X:%02X | RX %lu TX %lu | UP %lus\r\n",
               st->link_up ? "UP" : "DOWN",
               st->ip[0], st->ip[1], st->ip[2], st->ip[3],
               st->gw[0], st->gw[1], st->gw[2], st->gw[3],
               st->mac[0], st->mac[1], st->mac[2],
               st->mac[3], st->mac[4], st->mac[5],
               (unsigned long)st->rx_packets,
               (unsigned long)st->tx_packets,
               (unsigned long)st->link_uptime_s);
}

static void tcp_cmd_led(struct netconn *conn, const char *arg)
{
    if (strcmp(arg, "on") == 0) {
        BSP_LED_Set(0, 1);
        svc_write(conn, "LED ON\r\n");
    } else if (strcmp(arg, "off") == 0) {
        BSP_LED_Set(0, 0);
        svc_write(conn, "LED OFF\r\n");
    } else if (strcmp(arg, "toggle") == 0) {
        BSP_LED_Toggle(0);
        svc_write(conn, "LED TOGGLED\r\n");
    } else {
        svc_write(conn, "Usage: led <on|off|toggle>\r\n");
    }
}

static void tcp_cmd_beep(struct netconn *conn, const char *arg)
{
    unsigned long ms = 200;
    if (arg[0] != '\0') {
        char *end = NULL;
        ms = strtoul(arg, &end, 10);
        if (end == arg) {
            svc_write(conn, "Usage: beep <ms>\r\n");
            return;
        }
    }
    Buzzer_Beep((uint16_t)ms);
    svc_writef(conn, "BEEP %lu ms\r\n", ms);
}

static void tcp_cmd_mpu(struct netconn *conn)
{
    const imu_svc_state_t *s = ImuSvc_GetState();
    svc_writef(conn,
               "MPU: ready=%u samples=%lu faults=%lu\r\n"
               "R=%+.2f P=%+.2f Y=%+.2f deg\r\n"
               "A=(%+.3f,%+.3f,%+.3f)g G=(%+.2f,%+.2f,%+.2f)dps T=%+.1fC\r\n",
               (unsigned)s->ready, (unsigned long)s->sample_count,
               (unsigned long)s->fault_count,
               (double)s->roll, (double)s->pitch, (double)s->yaw,
               (double)s->ax, (double)s->ay, (double)s->az,
               (double)s->gx, (double)s->gy, (double)s->gz,
               (double)s->temp);
}

static void tcp_dispatch(struct netconn *conn, const char *line, uint8_t *stream_on)
{
    const char *args = line;
    while (*args == ' ') {
        args++;
    }
    char cmd[24];
    size_t n = 0;
    while (args[n] != '\0' && args[n] != ' ' && n < sizeof(cmd) - 1) {
        cmd[n] = args[n];
        n++;
    }
    cmd[n] = '\0';
    const char *rest = args + n;
    while (*rest == ' ') {
        rest++;
    }

    if (strcmp(cmd, "help") == 0) {
        tcp_cmd_help(conn);
    } else if (strcmp(cmd, "info") == 0) {
        tcp_cmd_info(conn);
    } else if (strcmp(cmd, "ver") == 0) {
        svc_writef(conn, "v%lu\r\n",
                   (unsigned long)(*(volatile uint32_t *)OTA_APP_VERSION_ADDR));
    } else if (strcmp(cmd, "sysmon") == 0) {
        tcp_cmd_sysmon(conn);
    } else if (strcmp(cmd, "taskstats") == 0) {
        tcp_cmd_taskstats(conn);
    } else if (strcmp(cmd, "net") == 0) {
        tcp_cmd_net(conn);
    } else if (strcmp(cmd, "led") == 0) {
        tcp_cmd_led(conn, rest);
    } else if (strcmp(cmd, "beep") == 0) {
        tcp_cmd_beep(conn, rest);
    } else if (strcmp(cmd, "mpu") == 0) {
        tcp_cmd_mpu(conn);
    } else if (strcmp(cmd, "echo") == 0) {
        svc_writef(conn, "%s\r\n", rest);
    } else if (strcmp(cmd, "stream") == 0) {
        if (strncmp(rest, "on", 2) == 0) {
            *stream_on = 1;
            svc_write(conn, "stream ON\r\n");
        } else if (strncmp(rest, "off", 3) == 0) {
            *stream_on = 0;
            svc_write(conn, "stream OFF\r\n");
        } else {
            svc_write(conn, "Usage: stream <on|off>\r\n");
        }
    } else if (cmd[0] != '\0') {
        svc_writef(conn, "Unknown: %s (type help)\r\n", cmd);
    }
}

/* ---------------- 客户端处理 ---------------- */

static void tcp_client_task(void *arg)
{
    struct netconn *conn = (struct netconn *)arg;
    char line[TCP_SVC_MAX_LINE];
    size_t line_len = 0;
    uint8_t stream_on = 0;

    svc_write(conn, "D00 Embedded Platform TCP Console\r\n");
    svc_write(conn, "Type 'help' for commands.\r\n> ");

    for (;;) {
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf(conn, &p);
        if (err == ERR_TIMEOUT) {
            if (stream_on) {
                tcp_stream_tick(conn);
                svc_write(conn, "> ");
            }
            continue;
        }
        if (err != ERR_OK || p == NULL) {
            break;
        }

        u16_t off = 0;
        while (off < p->tot_len) {
            size_t room = sizeof(line) - 1 - line_len;
            if (room == 0) {
                line_len = 0;   /* 超长行：丢弃重来 */
                room = sizeof(line) - 1;
            }
            u16_t take = (u16_t)((p->tot_len - off) < room
                                     ? (p->tot_len - off) : room);
            pbuf_copy_partial(p, line + line_len, take, off);
            line_len += take;
            off += take;

            for (;;) {
                char *nl = (char *)memchr(line, '\n', line_len);
                if (nl == NULL) {
                    break;
                }
                size_t l = (size_t)(nl - line);
                if (l > 0 && line[l - 1] == '\r') {
                    l--;
                }
                line[l] = '\0';
                tcp_dispatch(conn, line, &stream_on);
                size_t consumed = (size_t)(nl - line) + 1;
                line_len -= consumed;
                memmove(line, nl + 1, line_len);
            }
        }
        pbuf_free(p);
        svc_write(conn, "> ");

        /* 流模式用 1s 超时驱动遥测；普通模式 120s 空闲断开 */
        netconn_set_recvtimeout(conn, stream_on ? TCP_SVC_STREAM_MS
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
    osThreadAttr_t attr = {
        .name = "TcpSvc",
        .stack_size = TCP_SVC_TASK_STACK,
        .priority = osPriorityNormal,
    };
    s_server_task = osThreadNew(tcp_server_task, NULL, &attr);
    (void)s_server_task;
}
