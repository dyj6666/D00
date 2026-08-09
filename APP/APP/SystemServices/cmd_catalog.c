/* ================================================================
 * 命令目录：全部 cmd_* 实现（传输无关）
 *   - 命令只声明 transport 掩码（CMD_TRANSPORT_*），不感知物理协议
 *   - 输出统一走 LOG_Printf，由命令核心路由到当前适配器会话
 *   - 新增命令 = 在此加 cmd_xxx 实现 + cmd_table 一行
 * ================================================================ */
#include "cmd_catalog.h"
#include "bsp.h"
#include "logger.h"
#include "app_config.h"
#include "event_bus.h"
#include "la_sample.h"
#include "la_buffer.h"
#include "la_trigger.h"
#include "signal_gen.h"
#include "ota_agent.h"
#include "err_mgr.h"
#include "bsp_lcd.h"
#include "lcd_ui.h"
#include "lcd_test.h"
#include "touch_svc.h"
#include "bsp_touch.h"
#include "buzzer_app.h"
#include "imu_svc.h"
#include "eth_app.h"
#include "tcp_svc.h"
#include "icmp_svc.h"
#include "dns_svc.h"
#include "sntp_svc.h"
#include "mqtt_svc.h"
#include "http_svc.h"
#include "cmd_shell.h"
#include "usr_store.h"
#include "bsp_eeprom.h"
#include "bsp_mpu6050.h"
#include "bsp_i2c.h"
#include "i2c.h"
#include "lwip/ip4_addr.h"
#include "task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* cmd_func_t / cmd_entry_t provided by cmd_shell.h */
static void cmd_help(const char *args);
static void cmd_info(const char *args);
static void cmd_reset(const char *args);
static void cmd_led(const char *args);
static void cmd_taskstats(const char *args);
static void cmd_ota(const char *args);
static void cmd_sysmon(const char *args);
static void cmd_la_start(const char *args);
static void cmd_la_stop(const char *args);
static void cmd_la_trig(const char *args);
static void cmd_la_first(const char *args);
static void cmd_la_dma_start(const char *args);
static void cmd_la_dma_stop(const char *args);
static void cmd_la_dump(const char *args);
static void cmd_la_dma_stat(const char *args);
static void cmd_net(const char *args);
static void cmd_tcp(const char *args);
static void cmd_icmp(const char *args);
static void cmd_dhcp(const char *args);
static void cmd_dns(const char *args);
static void cmd_sntp(const char *args);
static void cmd_mqtt(const char *args);
static void cmd_http(const char *args);
static void cmd_la_info(const char *args);
static void cmd_la_state(const char *args);
static void cmd_la_peek(const char *args);
static void cmd_sg_uart_start(const char *args);
static void cmd_sg_uart_stop(const char *args);
static void cmd_sg_uart_hex(const char *args);
static void cmd_sg_spi_start(const char *args);
static void cmd_sg_spi_stop(const char *args);
static void cmd_sg_i2c_start(const char *args);
static void cmd_sg_i2c_stop(const char *args);
static void cmd_sg_i2c_complex(const char *args);
static void cmd_ota_rbtest(const char *args);
static void cmd_eb_stress(const char *args);
static void cmd_net(const char *args)
{
    EthApp_RefreshStatus();
    const eth_status_t *st = EthApp_GetStatus();
    if (args != NULL && strncmp(args, "ping", 4) == 0) {
        const char *ip = args + 4;
        while (*ip == ' ' || *ip == '\t') ip++;
        if (*ip == '\0') {
            LOG_Printf("Usage: net ping <ip>\r\n");
            return;
        }
        LOG_Printf("PING %s (timeout 2000ms)...\r\n", ip);
        int rtt = EthApp_Ping(ip, 2000);
        if (rtt >= 0) {
            LOG_Printf("Reply from %s: time=%dms\r\n", ip, rtt);
        } else {
            LOG_Printf("No reply from %s (err=%d)\r\n", ip, rtt);
        }
        return;
    }
    if (args != NULL && strncmp(args, "ip ", 3) == 0) {
        const char *ip = args + 3;
        while (*ip == ' ' || *ip == '\t') ip++;
        if (*ip == '\0') {
            LOG_Printf("Usage: net ip <a.b.c.d> | default\r\n");
            return;
        }
        if (strcmp(ip, "default") == 0) {
            EthApp_SetStaticIPDefault();
            LOG_Printf("IP restored to default 192.168.1.10/24, "
                       "saved config cleared\r\n");
            return;
        }
        if (EthApp_SetStaticIPPersist(ip) == 0) {
            LOG_Printf("IP set to %s/24 (saved to EEPROM)\r\n", ip);
        } else {
            LOG_Printf("Invalid IP: %s\r\n", ip);
        }
        return;
    }
    if (args != NULL && strncmp(args, "cap", 3) == 0) {
        const char *m = args + 3;
        while (*m == ' ' || *m == '\t') {
            m++;
        }
        if (strncmp(m, "on", 2) == 0) {
            ip4_addr_t peer;
            uint8_t peer4[4];
            int have_peer = 0;
            if (Cmd_ActiveTransport() == CMD_TRANSPORT_TCP) {
                tcp_cli_t *cli = (tcp_cli_t *)Cmd_ActiveUser();
                if (cli != NULL && TcpSvc_ClientPeerIP(cli, peer4) == 0) {
                    have_peer = 1;
                }
            }
            if (have_peer) {
                IP4_ADDR(&peer, peer4[0], peer4[1], peer4[2], peer4[3]);
                LOG_Printf("CAPTURE ON -> %u.%u.%u.%u:7778 (EthLab)\r\n",
                           (unsigned)peer4[0], (unsigned)peer4[1],
                           (unsigned)peer4[2], (unsigned)peer4[3]);
            } else {
                const char *ip = m + 2;
                while (*ip == ' ' || *ip == '\t') {
                    ip++;
                }
                if (*ip == '\0' || !ip4addr_aton(ip, &peer)) {
                    LOG_Printf("Usage: net cap on <a.b.c.d> (TCP auto-peer)\r\n");
                    return;
                }
                LOG_Printf("CAPTURE ON -> %s:7778 (EthLab)\r\n", ip);
            }
            EthApp_SetCapturePeer(&peer);
            EthApp_SetCapture(1);
        } else if (strncmp(m, "off", 3) == 0) {
            EthApp_SetCapture(0);
            LOG_Printf("CAPTURE OFF\r\n");
        } else {
            LOG_Printf("CAPTURE %s | SENT %lu DROP %lu\r\n",
                       EthApp_GetCaptureOn() ? "ON" : "OFF",
                       (unsigned long)EthApp_GetCapSent(),
                       (unsigned long)EthApp_GetCapDrop());
        }
        return;
    }
    if (args != NULL && strncmp(args, "udp ", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') p++;
        char ip[32];
        const char *sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        size_t ip_len = (size_t)(sp - p);
        if (ip_len == 0 || ip_len >= sizeof(ip)) {
            LOG_Printf("Usage: net udp <ip> <port> <hex>\r\n");
            return;
        }
        memcpy(ip, p, ip_len);
        ip[ip_len] = '\0';
        p = sp;
        while (*p == ' ' || *p == '\t') p++;
        char *endp = NULL;
        unsigned long port = strtoul(p, &endp, 10);
        if (endp == p) {
            LOG_Printf("Usage: net udp <ip> <port> <hex>\r\n");
            return;
        }
        p = endp;
        while (*p == ' ' || *p == '\t') p++;
        uint8_t buf[64];
        uint16_t blen = 0;
        while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            unsigned int v = 0;
            sscanf(p, "%2x", &v);
            if (blen < sizeof(buf)) buf[blen++] = (uint8_t)v;
            p += 2;
        }
        int r = EthApp_UdpSend(ip, (uint16_t)port, buf, blen);
        LOG_Printf("UDP %s:%lu len=%u -> %d\r\n", ip, port, (unsigned)blen, r);
        return;
    }
    if (args != NULL && strncmp(args, "dbg ", 4) == 0) {
        const char *m = args + 4;
        while (*m == ' ') m++;
        if (strcmp(m, "all") == 0 || strcmp(m, "tx") == 0 || strcmp(m, "1") == 0 || strcmp(m, "on") == 0) {
            EthApp_SetTxDbg(1);
            EthApp_SetRxDbg(strcmp(m, "all") == 0 ? 1 : 0);
            LOG_Printf("TX debug ON%s\r\n", strcmp(m, "all") == 0 ? ", RX debug ON" : "");
        } else if (strcmp(m, "rx") == 0) {
            EthApp_SetTxDbg(0);
            EthApp_SetRxDbg(1);
            LOG_Printf("RX debug ON\r\n");
        } else {
            EthApp_SetTxDbg(0);
            EthApp_SetRxDbg(0);
            LOG_Printf("frame debug OFF\r\n");
        }
        return;
    }
    LOG_Printf("=== ETH ===\r\n");
    LOG_Printf("  Link: %s\r\n", st->link_up ? "UP" : "DOWN");
    if (st->link_up) {
        LOG_Printf("  IP  : %u.%u.%u.%u\r\n", st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
        LOG_Printf("  GW  : %u.%u.%u.%u\r\n", st->gw[0], st->gw[1], st->gw[2], st->gw[3]);
        LOG_Printf("  MAC : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5]);
        LOG_Printf("  RX  : %lu packets\r\n", (unsigned long)st->rx_packets);
        LOG_Printf("  TX  : %lu packets\r\n", (unsigned long)st->tx_packets);
        LOG_Printf("  UP  : %lu s\r\n", (unsigned long)st->link_uptime_s);
    }
}
static void cmd_lcd(const char *args);
static void cmd_touch(const char *args);
static void cmd_beep(const char *args);
static void cmd_mpu(const char *args);
static void cmd_ver(const char *args);
static void cmd_echo(const char *args);
static void cmd_stream(const char *args);
static void cmd_usr(const char *args);
#if CRASH_INJECT_ENABLE
static void cmd_crash(const char *args);
#endif

static void cmd_tcp(const char *args)
{
    (void)args;
    const tcp_svc_stat_t *st = TcpSvc_GetStat();
    LOG_Printf("TCP console: port 9000, clients=%lu accepted=%lu rejected=%lu\r\n",
               (unsigned long)st->clients,
               (unsigned long)st->accepted,
               (unsigned long)st->rejected);
}

static void cmd_icmp(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        const icmp_svc_stat_t *st = IcmpSvc_GetStat();
        LOG_Printf("ICMP service: %s, limit=%lu pps, uptime=%lus\r\n",
                   st->enabled ? "reply ON" : "reply OFF (silent)",
                   (unsigned long)st->rate_limit_pps,
                   (unsigned long)st->uptime_s);
        LOG_Printf("  rx echo=%lu reply=%lu drop=%lu other=%lu total=%lu\r\n",
                   (unsigned long)st->echo_rx,
                   (unsigned long)st->echo_tx,
                   (unsigned long)st->echo_drop,
                   (unsigned long)st->other_rx,
                   (unsigned long)st->total_rx);
        LOG_Printf("  rate=%lu pps (peak %lu)  rtt min/avg/max=%lu/%lu/%lu us\r\n",
                   (unsigned long)st->rate_pps,
                   (unsigned long)st->peak_pps,
                   (unsigned long)st->min_rtt_us,
                   (unsigned long)st->avg_rtt_us,
                   (unsigned long)st->max_rtt_us);
        LOG_Printf("  last peer=%u.%u.%u.%u seq=%u\r\n",
                   st->last_peer[0], st->last_peer[1],
                   st->last_peer[2], st->last_peer[3],
                   (unsigned)st->last_seq);
        return;
    }
    if (strncmp(args, "reset", 5) == 0) {
        IcmpSvc_Reset();
        LOG_Printf("ICMP: stats reset\r\n");
        return;
    }
    if (strncmp(args, "reply", 5) == 0) {
        const char *p = args + 5;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "on", 2) == 0) {
            IcmpSvc_SetEnabled(1);
            LOG_Printf("ICMP: reply ON\r\n");
        } else if (strncmp(p, "off", 3) == 0) {
            IcmpSvc_SetEnabled(0);
            LOG_Printf("ICMP: reply OFF (silent)\r\n");
        } else {
            LOG_Printf("Usage: icmp reply <on|off>\r\n");
        }
        return;
    }
    if (strncmp(args, "limit", 5) == 0) {
        int pps = atoi(args + 5);
        if (pps > 0 && pps <= 65535) {
            IcmpSvc_SetRateLimit((uint16_t)pps);
            LOG_Printf("ICMP: rate limit=%d pps\r\n", pps);
        } else {
            LOG_Printf("Usage: icmp limit <1..65535>\r\n");
        }
        return;
    }
    LOG_Printf("Usage: icmp <info|reset|reply <on|off>|limit <pps>>\r\n");
}

static void cmd_dhcp(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "status", 6) == 0) {
        LOG_Printf("DHCP: %s\r\n", EthApp_DhcpState());
        return;
    }
    if (strncmp(args, "on", 2) == 0) {
        EthApp_DhcpStart();
        LOG_Printf("DHCP: starting, fallback %us\r\n",
                   (unsigned)(ETH_DHCP_FALLBACK_MS / 1000u));
        return;
    }
    if (strncmp(args, "off", 3) == 0) {
        EthApp_DhcpStop();
        LOG_Printf("DHCP: stopped, static IP restored\r\n");
        return;
    }
    LOG_Printf("Usage: dhcp <on|off|status>\r\n");
}

static void cmd_dns(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        const uint8_t *s = DnsSvc_GetServer();
        if (s != NULL) {
            LOG_Printf("DNS: server %u.%u.%u.%u\r\n",
                       (unsigned)s[0], (unsigned)s[1],
                       (unsigned)s[2], (unsigned)s[3]);
        } else {
            LOG_Printf("DNS: no server configured\r\n");
        }
        return;
    }
    if (strncmp(args, "server", 6) == 0) {
        const char *ip = args + 6;
        while (*ip == ' ' || *ip == '\t') ip++;
        if (*ip == '\0') {
            LOG_Printf("Usage: dns server <ip>\r\n");
            return;
        }
        LOG_Printf("DNS: server set -> %d\r\n", DnsSvc_SetServer(ip));
        return;
    }
    if (strncmp(args, "resolve", 7) == 0) {
        const char *h = args + 7;
        while (*h == ' ' || *h == '\t') h++;
        if (*h == '\0') {
            LOG_Printf("Usage: dns resolve <host>\r\n");
            return;
        }
        LOG_Printf("DNS: resolving %s ...\r\n", h);
        uint8_t ip[4];
        int r = DnsSvc_Resolve(h, 3000u, ip);
        if (r == 0) {
            LOG_Printf("DNS: %s = %u.%u.%u.%u\r\n",
                       h, (unsigned)ip[0], (unsigned)ip[1],
                       (unsigned)ip[2], (unsigned)ip[3]);
        } else {
            LOG_Printf("DNS: %s -> err=%d\r\n", h, r);
        }
        return;
    }
    LOG_Printf("Usage: dns <info|server <ip>|resolve <host>>\r\n");
}

static void cmd_sntp(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        const uint8_t *s = SntpSvc_GetServer();
        char ts[32];
        SntpSvc_GetTimeStr(ts, sizeof(ts));
        if (s != NULL) {
            LOG_Printf("SNTP: server %u.%u.%u.%u auto=%u\r\n",
                       (unsigned)s[0], (unsigned)s[1],
                       (unsigned)s[2], (unsigned)s[3],
                       (unsigned)SntpSvc_Auto());
        } else {
            LOG_Printf("SNTP: no server, auto=%u\r\n",
                       (unsigned)SntpSvc_Auto());
        }
        LOG_Printf("SNTP: RTC %s\r\n", ts);
        return;
    }
    if (strncmp(args, "sync", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') p++;
        const uint8_t *srv = SntpSvc_GetServer();
        uint8_t local[4];
        uint16_t port = 123u;
        if (*p != '\0') {
            const char *sp = p;
            char ip[32];
            char *dst = ip;
            while (*sp != '\0' && *sp != ' ' && *sp != '\t' &&
                   (dst - ip) < 31) {
                *dst++ = *sp++;
            }
            *dst = '\0';
            if (*sp != '\0') {
                port = (uint16_t)atoi(sp);
            }
            if (SntpSvc_SetServer(ip) != 0) {
                LOG_Printf("SNTP: invalid server %s\r\n", ip);
                return;
            }
            srv = SntpSvc_GetServer();
        }
        if (srv == NULL) {
            LOG_Printf("SNTP: no server (use sntp sync <ip>)\r\n");
            return;
        }
        memcpy(local, srv, 4);
        LOG_Printf("SNTP: syncing %u.%u.%u.%u:%u ...\r\n",
                   (unsigned)local[0], (unsigned)local[1],
                   (unsigned)local[2], (unsigned)local[3],
                   (unsigned)port);
        int r = SntpSvc_Sync(local, port, 3000u);
        char ts[32];
        SntpSvc_GetTimeStr(ts, sizeof(ts));
        LOG_Printf("SNTP: %s, RTC=%s\r\n", (r == 0) ? "OK" : "FAIL", ts);
        return;
    }
    if (strncmp(args, "auto", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "on", 2) == 0) {
            SntpSvc_SetAuto(1);
            LOG_Printf("SNTP: auto ON\r\n");
        } else if (strncmp(p, "off", 3) == 0) {
            SntpSvc_SetAuto(0);
            LOG_Printf("SNTP: auto OFF\r\n");
        } else {
            LOG_Printf("Usage: sntp auto <on|off>\r\n");
        }
        return;
    }
    LOG_Printf("Usage: sntp <info|sync [server [port]]|auto <on|off>>\r\n");
}

static void cmd_mqtt(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        const mqtt_svc_stat_t *st = MqttSvc_GetStat();
        static const char *state_str[] = {"IDLE", "CONNECTING", "CONNECTED", "ERR"};
        const char *st_s = (st->state < 4u) ? state_str[st->state] : "?";
        LOG_Printf("MQTT: state=%s client=%s\r\n", st_s, st->client_id);
        LOG_Printf("MQTT: broker %u.%u.%u.%u:%u\r\n",
                   (unsigned)st->broker[0], (unsigned)st->broker[1],
                   (unsigned)st->broker[2], (unsigned)st->broker[3],
                   (unsigned)st->port);
        LOG_Printf("MQTT: conn=%lu disc=%lu pub=%lu sub=%lu err=%lu\r\n",
                   (unsigned long)st->connect_cnt,
                   (unsigned long)st->disconnect_cnt,
                   (unsigned long)st->pub_cnt,
                   (unsigned long)st->sub_cnt,
                   (unsigned long)st->err_cnt);
        return;
    }
    if (strncmp(args, "connect", 7) == 0) {
        const char *p = args + 7;
        while (*p == ' ' || *p == '\t') p++;
        char ip[32];
        uint16_t port = 0;
        if (*p != '\0') {
            const char *sp = p;
            char *dst = ip;
            while (*sp != '\0' && *sp != ' ' && *sp != '\t' &&
                   (dst - ip) < 31) {
                *dst++ = *sp++;
            }
            *dst = '\0';
            if (*sp != '\0') port = (uint16_t)atoi(sp);
            int r = MqttSvc_Connect(ip, port);
            LOG_Printf("MQTT: connect %s -> %d\r\n", ip, r);
        } else {
            LOG_Printf("MQTT: connect -> %d\r\n", MqttSvc_Connect(NULL, 0));
        }
        return;
    }
    if (strncmp(args, "disconnect", 10) == 0) {
        MqttSvc_Disconnect();
        LOG_Printf("MQTT: disconnecting\r\n");
        return;
    }
    if (strncmp(args, "pub", 3) == 0) {
        const char *p = args + 3;
        while (*p == ' ' || *p == '\t') p++;
        const char *topic = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
        if (topic == p) {
            LOG_Printf("Usage: mqtt pub <topic> <data>\r\n");
            return;
        }
        char tbuf[48];
        int tl = (int)(p - topic);
        if (tl > 47) tl = 47;
        memcpy(tbuf, topic, tl);
        tbuf[tl] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        LOG_Printf("MQTT: pub %s -> %d\r\n", tbuf,
                   MqttSvc_Publish(tbuf, *p ? p : ""));
        return;
    }
    if (strncmp(args, "sub", 3) == 0) {
        const char *p = args + 3;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            LOG_Printf("Usage: mqtt sub <topic>\r\n");
            return;
        }
        LOG_Printf("MQTT: sub %s -> %d\r\n", p, MqttSvc_Subscribe(p));
        return;
    }
    LOG_Printf("Usage: mqtt <connect [ip] [port]|disconnect|pub <t> <d>|sub <t>|info>\r\n");
}

static void cmd_http(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        LOG_Printf("HTTP: %s, requests=%lu, port=8080\r\n",
                   HttpSvc_Enabled() ? "ON" : "OFF",
                   (unsigned long)HttpSvc_GetRequests());
        return;
    }
    if (strncmp(args, "on", 2) == 0) {
        HttpSvc_SetEnabled(1);
        LOG_Printf("HTTP: ON\r\n");
        return;
    }
    if (strncmp(args, "off", 3) == 0) {
        HttpSvc_SetEnabled(0);
        LOG_Printf("HTTP: OFF\r\n");
        return;
    }
    LOG_Printf("Usage: http <info|on|off>\r\n");
}

static const cmd_entry_t cmd_table[] = {
    {"help",         "Show command help", CMD_TRANSPORT_ALL, cmd_help},
    {"info",         "System info (version/kernel/tasks)", CMD_TRANSPORT_ALL, cmd_info},
    {"reset",        "Software reset", CMD_TRANSPORT_ALL, cmd_reset},
    {"led",          "LED control (on/off/toggle/blink)", CMD_TRANSPORT_ALL, cmd_led},
    {"taskstats",    "Task list & stack usage", CMD_TRANSPORT_ALL, cmd_taskstats},
    {"ota",          "Enter BOOT upgrade mode", CMD_TRANSPORT_UART, cmd_ota},
    {"sysmon",       "System monitor report", CMD_TRANSPORT_ALL, cmd_sysmon},
    {"la_start",     "LA start (timestamp mode)", CMD_TRANSPORT_ALL, cmd_la_start},
    {"la_stop",      "LA stop", CMD_TRANSPORT_ALL, cmd_la_stop},
    {"la_first",     "Show first sample", CMD_TRANSPORT_ALL, cmd_la_first},
    {"la_trig",      "Configure trigger", CMD_TRANSPORT_ALL, cmd_la_trig},
    {"la_dma_start", "Start DMA sampling <rate>", CMD_TRANSPORT_ALL, cmd_la_dma_start},
    {"la_dma_stop",  "Stop DMA sampling", CMD_TRANSPORT_ALL, cmd_la_dma_stop},
    {"la_dump",      "Export samples <count>", CMD_TRANSPORT_ALL, cmd_la_dump},
    {"la_dma_stat",  "DMA sampling stats", CMD_TRANSPORT_ALL, cmd_la_dma_stat},
    {"la_info",      "LA info", CMD_TRANSPORT_ALL, cmd_la_info},
    {"la_state",     "LA state", CMD_TRANSPORT_ALL, cmd_la_state},
    {"la_peek",      "Peek sample at index", CMD_TRANSPORT_ALL, cmd_la_peek},
    {"sg_uart_start", "UART generator <baud> <text> <ms>", CMD_TRANSPORT_ALL, cmd_sg_uart_start},
    {"tcp",          "TCP console status (port 9000)", CMD_TRANSPORT_ALL, cmd_tcp},
    {"net",          "ETH status / ping <ip>", CMD_TRANSPORT_ALL, cmd_net},
    {"icmp",         "ICMP service <info|reset|reply on|off|limit pps>", CMD_TRANSPORT_ALL, cmd_icmp},
    {"dhcp",         "DHCP client <on|off|status>", CMD_TRANSPORT_ALL, cmd_dhcp},
    {"dns",          "DNS <info|server <ip>|resolve <host>>", CMD_TRANSPORT_ALL, cmd_dns},
    {"sntp",         "SNTP <info|sync [server]|auto on|off>", CMD_TRANSPORT_ALL, cmd_sntp},
    {"mqtt",         "MQTT <connect [ip] [port]|disconnect|pub <t> <d>|sub <t>|info>", CMD_TRANSPORT_ALL, cmd_mqtt},
    {"http",         "HTTP status server <info|on|off>", CMD_TRANSPORT_ALL, cmd_http},
    {"sg_uart_stop", "Stop UART generator", CMD_TRANSPORT_ALL, cmd_sg_uart_stop},
    {"sg_uart_hex",  "UART hex frame generator", CMD_TRANSPORT_ALL, cmd_sg_uart_hex},
    {"sg_spi_start", "SPI generator <hex> <ms>", CMD_TRANSPORT_ALL, cmd_sg_spi_start},
    {"sg_spi_stop",  "Stop SPI generator", CMD_TRANSPORT_ALL, cmd_sg_spi_stop},
    {"sg_i2c_start", "I2C generator <addr> <hex> <ms>", CMD_TRANSPORT_ALL, cmd_sg_i2c_start},
    {"sg_i2c_stop",  "Stop I2C generator", CMD_TRANSPORT_ALL, cmd_sg_i2c_stop},
    {"sg_i2c_complex","I2C complex frame demo", CMD_TRANSPORT_ALL, cmd_sg_i2c_complex},
    {"ota_rbtest",   "OTA rollback self-test (danger)", CMD_TRANSPORT_UART, cmd_ota_rbtest},
    {"eb_stress",    "Event bus stress <n> <payload> <mode>", CMD_TRANSPORT_ALL, cmd_eb_stress},
    {"lcd",          "LCD <info|page <0-5>|test|clear|bench|dir|bl>", CMD_TRANSPORT_ALL, cmd_lcd},
    {"touch",        "Touch <info|cal|test>", CMD_TRANSPORT_ALL, cmd_touch},
    {"usr",          "User storage <info|scan|get|set|erase|reset>", CMD_TRANSPORT_ALL, cmd_usr},
    {"beep",         "Buzzer beep <ms|test|off>", CMD_TRANSPORT_ALL, cmd_beep},
    {"mpu",          "IMU MPU6050 <info|test|cal>", CMD_TRANSPORT_ALL, cmd_mpu},
#if CRASH_INJECT_ENABLE
    {"crash",        "Crash injection test <bus|undef|stack|assert|irq>", CMD_TRANSPORT_UART, cmd_crash},
#endif

    {"ver",          "Firmware version", CMD_TRANSPORT_ALL, cmd_ver},
    {"echo",         "Echo text (connectivity test)", CMD_TRANSPORT_ALL, cmd_echo},
    {"stream",       "Telemetry stream <on|off> (TCP)", CMD_TRANSPORT_TCP, cmd_stream},
};
#define CMD_COUNT (sizeof(cmd_table) / sizeof(cmd_table[0]))

/* ================== 命令实现 ================== */
static void cmd_help(const char *args)
{
    if (args != NULL && *args != '\0') {
        for (uint32_t i = 0; i < Cmd_Count(); i++) {
            const cmd_entry_t *e = Cmd_Get(i);
            if (e != NULL && strcmp(e->name, args) == 0) {
                LOG_Printf("%s - %s\r\n", e->name, e->brief);
                return;
            }
        }
        LOG_Printf("Unknown command: %s\r\n", args);
        return;
    }
    Cmd_Help(NULL);
    LOG_Printf("Tip: Tab = complete, Up/Down = history\r\n");
}

static void cmd_info(const char *args)
{
    (void)args;
    LOG_Printf("STM32F407ZGT6 @ 168MHz\r\n");
    LOG_Printf("FreeRTOS %s\r\n", tskKERNEL_VERSION_NUMBER);
    LOG_Printf("Tasks: %ld\r\n", uxTaskGetNumberOfTasks());
    LOG_Printf("Free heap: %lu bytes\r\n", (unsigned long)xPortGetFreeHeapSize());
}

static void cmd_ver(const char *args)
{
    (void)args;
    LOG_Printf("v%lu\r\n",
               (unsigned long)(*(volatile uint32_t *)OTA_APP_VERSION_ADDR));
}

static void cmd_echo(const char *args)
{
    LOG_Printf("%s\r\n", args != NULL ? args : "");
}

static void cmd_stream(const char *args)
{
    if (Cmd_ActiveTransport() != CMD_TRANSPORT_TCP) {
        LOG_Printf("stream: TCP console only\r\n");
        return;
    }
    tcp_cli_t *cli = (tcp_cli_t *)Cmd_ActiveUser();
    if (args != NULL && strncmp(args, "on", 2) == 0) {
        if (TcpSvc_ClientSetStream(cli, 1) == 0) {
            LOG_Printf("stream ON\r\n");
        }
    } else if (args != NULL && strncmp(args, "off", 3) == 0) {
        if (TcpSvc_ClientSetStream(cli, 0) == 0) {
            LOG_Printf("stream OFF\r\n");
        }
    } else {
        LOG_Printf("Usage: stream <on|off>\r\n");
    }
}

static void cmd_reset(const char *args)
{
    (void)args;
    LOG_Printf("Resetting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(10));
    BSP_SystemReset();
}

static void cmd_led(const char *args)
{
    if (args == NULL) {
        LOG_Printf("Usage: led on/off/toggle\r\n");
        return;
    }
    MSG_SEND_DATA(MODULE_SHELL, MSG_CMD_LED, args, strlen(args) + 1);
}

static void cmd_taskstats(const char *args)
{
    (void)args;
    UBaseType_t size = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = pvPortMalloc(size * sizeof(TaskStatus_t));
    if (arr == NULL) {
        LOG_Printf("taskstats: no memory\r\n");
        return;
    }
    size = uxTaskGetSystemState(arr, size, NULL);
    LOG_Printf("Task\tState\tPrio\tStack\t#\r\n");
    for (UBaseType_t i = 0; i < size; i++) {
        char st = 'X';
        switch (arr[i].eCurrentState) {
            case eRunning:   st = 'R'; break;
            case eBlocked:   st = 'B'; break;
            case eSuspended: st = 'S'; break;
            case eDeleted:   st = 'D'; break;
            default:         break;
        }
        LOG_Printf("%-12s\t%c\t%u\t%u\t%u\r\n",
                   arr[i].pcTaskName, st,
                   (unsigned)arr[i].uxCurrentPriority,
                   (unsigned)arr[i].usStackHighWaterMark,
                   (unsigned)arr[i].xTaskNumber);
    }
    vPortFree(arr);
    LOG_Printf("Free heap: %lu bytes\r\n", (unsigned long)xPortGetFreeHeapSize());
}

static void cmd_ota(const char *args)
{
    (void)args;
    LOG_Printf("OTA command received, publishing event...\r\n");
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_OTA_START);
}

static void cmd_sysmon(const char *args)
{
    (void)args;
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_SYSMON);
}

static void cmd_la_start(const char *args)
{
    (void)args;
    LA_Sample_Start(LA_MODE_TIMESTAMP);
    LOG_Printf("LA started\r\n");

    LA_Diag_PrintExtiStatus();
}

static void cmd_la_stop(const char *args)
{
    (void)args;
    LA_Sample_Stop();
    LOG_Printf("LA stopped, samples: %lu\r\n", LA_Buffer_GetCount());
}

static void cmd_la_first(const char *args)
{
    (void)args;
    if (LA_Buffer_GetCount() == 0) {
        LOG_Printf("No samples\r\n");
        return;
    }

    LA_SamplePoint pt;
    LA_Buffer_Read(&pt, 0, 1);
    uint32_t ts = ((uint32_t)pt.timestamp_hi << 16) | pt.timestamp_lo;
    LOG_Printf("First: ts=%lu, states=0x%02X\r\n", ts, pt.states);
}

static void cmd_la_trig(const char *args)
{
    /* 格式：la_trig <type> <ch> [post] [cond_ch] [cond_level]
       type: 0=off 1=rising 2=falling 3=any
       post: 触发后采样点数（默认 2048）
       cond_ch/cond_level: 条件通道与电平（可选，如 I2C START）
       la_trig 2 0 2048 1 1 = CH0 下降沿且 CH1 为高时触发） */
    la_trigger_cfg_t cfg;
    LA_Trigger_GetConfig(&cfg);
    int type = 0, channel = 0, post = 0, cond_ch = -1, cond_level = 1;
    if (args) {
        int n = sscanf(args, "%d %d %d %d %d", &type, &channel, &post, &cond_ch, &cond_level);
        if (n >= 3 && post > 0) cfg.post_samples = (uint16_t)post;
        if (n >= 4 && cond_ch >= 0 && cond_ch < LA_MAX_CHANNELS) {
            cfg.cond_channel = (uint8_t)cond_ch;
            cfg.cond_level = (uint8_t)(cond_level != 0);
        }
    }
    cfg.type = (LA_TriggerType)type;
    cfg.channel = (uint8_t)channel;
    LA_Trigger_SetConfig(&cfg);

    if (cfg.type == LA_TRIG_NONE) {
        LOG_Printf("Trigger off\r\n");
    } else {
        LOG_Printf("Trigger: type=%d, ch=%d, post=%u",
                   cfg.type, cfg.channel, (unsigned)cfg.post_samples);
        if (cfg.cond_channel != 0xFF) {
            LOG_Printf(", cond=ch%d==%d", cfg.cond_channel, cfg.cond_level);
        }
        LOG_Printf("\r\n");
    }
}

static void cmd_la_dma_start(const char *args)
{
    uint32_t rate = 100000;     /* 默认 100kHz */
    if (args) rate = atoi(args);
    LA_Sample_Start_DMA(rate);
}

static void cmd_la_dma_stop(const char *args)
{
    (void)args;
    uint32_t cnt = LA_Sample_Stop_DMA();
    LOG_Printf("DMA capture stopped, samples: %lu\r\n", cnt);

    if (cnt > 0) {
        uint32_t buf[4];
        LA_Sample_ReadDMABuffer(buf, 0, 4);
        LOG_Printf("First 4: %08lX %08lX %08lX %08lX\r\n",
                   buf[0], buf[1], buf[2], buf[3]);
    }
}

static void cmd_la_dump(const char *args)
{
    /* 格式：la_dump <count>（默认 512，上限为缓冲深度），导出 DMA 采样值*/
    uint32_t count = 512;
    if (args) count = (uint32_t)atoi(args);
    if (count == 0) count = 1;
    uint32_t cap = LA_Sample_GetDMABufferSize();
    if (count > cap) count = cap;
    if (count > 4096) {
        LOG_Printf("Dumping %lu samples, this will take a while...\r\n",
                   (unsigned long)count);
    }

    LOG_Printf("Dump %lu samples from DMA buffer:\r\n", (unsigned long)count);
    uint32_t buf[8];
    for (uint32_t i = 0; i < count; i += 8) {
        for (int k = 0; k < 8; k++) buf[k] = 0;
        uint32_t n = (count - i < 8) ? (count - i) : 8;
        LA_Sample_ReadDMABuffer(buf, i, n);
        LOG_Printf("%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
                   (unsigned long)buf[0], (unsigned long)buf[1],
                   (unsigned long)buf[2], (unsigned long)buf[3],
                   (unsigned long)buf[4], (unsigned long)buf[5],
                   (unsigned long)buf[6], (unsigned long)buf[7]);
        /* 限速：日志 TX 按 115200 波特率排空（约 11.5 KB/s），
         * 不延时会导致 2 KB 流缓冲灌满并静默丢帧 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void cmd_la_dma_stat(const char *args)
{
    (void)args;
    LOG_Printf("DMA stat: count=%lu, buf=%lu pts, overrun=%u, src=%s\r\n",
               (unsigned long)LA_Sample_GetDMACount(),
               (unsigned long)LA_Sample_GetDMABufferSize(),
               LA_Sample_GetDMAOverrun(),
               "SRAM");
}


static void cmd_la_info(const char *args)
{
    la_trigger_cfg_t cfg;
    (void)args;
    LA_Trigger_GetConfig(&cfg);
    LOG_Printf("=== LA INFO ===\r\n");
    LOG_Printf("  DMA buffer: %lu pts (%s)\r\n",
               (unsigned long)LA_Sample_GetDMABufferSize(),
               "external SRAM");
    LOG_Printf("  SRAM self-test: %s\r\n", LA_Buffer_IsSramOk() ? "PASS" : "FAIL");
    LOG_Printf("  Trigger: type=%d ch=%d post=%u cond=",
               cfg.type, cfg.channel, (unsigned)cfg.post_samples);
    if (cfg.cond_channel == 0xFF) {
        LOG_Printf("none\r\n");
    } else {
        LOG_Printf("ch%d==%d\r\n", cfg.cond_channel, cfg.cond_level);
    }
}

static void cmd_la_state(const char *args)
{
    (void)args;
    uint8_t states = LA_Sample_GetChannelStates();
    LOG_Printf("CH states: 0x%02X (CH0=%d CH1=%d CH2=%d CH3=%d)\r\n",
               states, (states >> 0) & 1, (states >> 1) & 1,
               (states >> 2) & 1, (states >> 3) & 1);
}

static void cmd_la_peek(const char *args)
{
    (void)args;
    uint8_t states = LA_Sample_GetChannelStates();
    LOG_Printf("states=0x%02X, ch0=%d, ch3=%lu, la_samples=%lu\r\n",
               states, (states & 0x01) ? 1 : 0,
               (unsigned long)la_ch3_state, la_samples);
}

static void cmd_sg_uart_start(const char *args)
{
    uint32_t baud = 115200;
    char text[SG_TEXT_MAX] = "HELLO";
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%lu %63s %d", &baud, text, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_UartStart(baud, text, (uint16_t)interval);
    LOG_Printf("SG UART: %s (baud=%lu text=%s interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned long)baud, text, interval);
}

static void cmd_sg_uart_stop(const char *args)
{
    (void)args;
    SG_UartStop();
    LOG_Printf("SG UART: stopped\r\n");
}

static void cmd_sg_uart_hex(const char *args)
{
    uint32_t baud = 115200;
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%lu %127s %d", &baud, hex, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_UartStartHex(baud, hex, (uint16_t)interval);
    LOG_Printf("SG UART HEX: %s (baud=%lu len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned long)baud, strlen(hex) / 2, interval);
}

static void cmd_sg_spi_start(const char *args)
{
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%127s %d", hex, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_SpiStartHex(hex, (uint16_t)interval);
    LOG_Printf("SG SPI: %s (freq=164kHz mode0 len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED", strlen(hex) / 2, interval);
}

static void cmd_sg_spi_stop(const char *args)
{
    (void)args;
    SG_SpiStop();
    LOG_Printf("SG SPI: stopped\r\n");
}

static void cmd_sg_i2c_start(const char *args)
{
    unsigned addr = 0x50;
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%x %127s %d", &addr, hex, &interval);
    }
    if (interval < 1) interval = 1;
    if (addr > 0x7F) addr = 0x50;
    int ret = SG_I2CStart((uint8_t)addr, hex, (uint16_t)interval);
    LOG_Printf("SG I2C: %s (addr=0x%02X len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned)addr, strlen(hex) / 2, interval);
}

static void cmd_sg_i2c_stop(const char *args)
{
    (void)args;
    SG_I2CStop();
    LOG_Printf("SG I2C: stopped\r\n");
}

static void cmd_sg_i2c_complex(const char *args)
{
    unsigned addr = 0x50;
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%x %d", &addr, &interval);
    }
    if (interval < 1) interval = 1;
    if (addr > 0x7F) addr = 0x50;
    int ret = SG_I2CComplexStart((uint8_t)addr, (uint16_t)interval);
    LOG_Printf("SG I2C COMPLEX: %s (addr=0x%02X interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED", (unsigned)addr, interval);
}

static void cmd_ota_rbtest(const char *args)
{
    (void)args;
    LOG_Printf("OTA rollback test: arming...\r\n");
    Ota_ForceRollbackTest();
}

/* ================== IMU 命令 ==================
 * 用法：mpu <info|test|cal> */
static void cmd_mpu(const char *args)
{
    const imu_svc_state_t *s = ImuSvc_GetState();
    if (args == NULL || strcmp(args, "info") == 0) {
        LOG_Printf("MPU: ready=%u samples=%lu faults=%lu\r\n",
                   (unsigned)s->ready, (unsigned long)s->sample_count,
                   (unsigned long)s->fault_count);
        LOG_Printf("MPU: R=%+.2f P=%+.2f Y=%+.2f deg\r\n",
                   (double)s->roll, (double)s->pitch, (double)s->yaw);
        LOG_Printf("MPU: A=(%+.3f,%+.3f,%+.3f)g G=(%+.2f,%+.2f,%+.2f)dps T=%+.1fC\r\n",
                   (double)s->ax, (double)s->ay, (double)s->az,
                   (double)s->gx, (double)s->gy, (double)s->gz,
                   (double)s->temp);
        return;
    }
    if (strcmp(args, "test") == 0) {
        LOG_Printf("MPU: streaming R/P/Y and G for 2s...\r\n");
        for (int i = 0; i < 20; i++) {
            LOG_Printf("MPU: %+8.2f %+8.2f %+8.2f | %+6.1f %+6.1f %+6.1f\r\n",
                       (double)s->roll, (double)s->pitch, (double)s->yaw,
                       (double)s->gx, (double)s->gy, (double)s->gz);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return;
    }
    if (strcmp(args, "cal") == 0) {
        ImuSvc_Recalibrate();
        return;
    }
    LOG_Printf("Usage: mpu <info|test|cal>\r\n");
}
/* ================== 蜂鸣器命令 ==================
 * 用法：beep [<ms>|test|off] */
static void cmd_beep(const char *args)
{
    if (args == NULL || strcmp(args, "test") == 0) {
        Buzzer_BeepPattern(2, 80, 60);
        LOG_Printf("BEEP: double beep\r\n");
        return;
    }
    if (strcmp(args, "off") == 0) {
        Buzzer_Stop();
        LOG_Printf("BEEP: stopped\r\n");
        return;
    }
    int ms = atoi(args);
    if (ms < 1 || ms > 3000) ms = 100;
    Buzzer_Beep((uint16_t)ms);
    LOG_Printf("BEEP: %d ms\r\n", ms);
}
/* ================== 触摸屏命令 ==================
 * 用法：touch <info|cal|test> */
static void cmd_touch(const char *args)
{
    if (args == NULL || strcmp(args, "info") == 0) {
        const touch_svc_state_t *ts = TouchSvc_GetState();
        bsp_touch_cal_t cal;
        BSP_Touch_GetCal(&cal);
        LOG_Printf("TOUCH: state=%u pos=%u,%u raw=%u,%u gen=%lu\r\n",
                   (unsigned)ts->state, (unsigned)ts->x, (unsigned)ts->y,
                   (unsigned)ts->raw_x, (unsigned)ts->raw_y,
                   (unsigned long)ts->gen);
        LOG_Printf("TOUCH: cal xfac=%ld yfac=%ld xc=%ld yc=%ld valid=%u\r\n",
                   (long)cal.xfac, (long)cal.yfac, (long)cal.xc, (long)cal.yc,
                   (unsigned)cal.valid);
        int32_t frx = 0, fry = 0;
        uint8_t fok = BSP_Touch_ReadRawForce(&frx, &fry);
        LOG_Printf("TOUCH: probe ok=%u raw=%ld,%ld\r\n",
                   (unsigned)fok, (long)frx, (long)fry);
        return;

    }
    if (strncmp(args, "nudge", 5) == 0) {
        const char *p = args + 5;
        int dx = atoi(p);
        while (*p == ' ' || (*p >= '0' && *p <= '9') || *p == '-') p++;
        int dy = atoi(p);
        bsp_touch_cal_t cal;
        BSP_Touch_GetCal(&cal);
        if (cal.xfac != 0) cal.xc += (int32_t)dx * cal.xfac;
        if (cal.yfac != 0) cal.yc += (int32_t)dy * cal.yfac;
        BSP_Touch_SetCal(&cal);
        (void)UsrStore_Set(USR_KEY_TOUCH_CAL, &cal, sizeof(cal));
        LOG_Printf("TOUCH: nudge (%d,%d) -> xc=%ld yc=%ld\r\n",
                   dx, dy, (long)cal.xc, (long)cal.yc);
        return;
    }
    if (strcmp(args, "cal") == 0) {
        TouchSvc_Calibrate();
        return;
    }
    if (strcmp(args, "test") == 0) {
        LcdUI_ShowPage(3);   /* TOUCH 测试ҳ */
        LOG_Printf("TOUCH: test page shown, touch the screen\r\n");
        return;
    }
    LOG_Printf("Usage: touch <info|cal|nudge <dx> <dy>|test>\r\n");
}
/* ================== LCD 测试命令 ==================
 * 用法：lcd <info|test|clear|bench|dir <0-7>|bl <0|1>>
 * 所有测试绘制均在 LcdUI 渲Ⱦ任务内串行ִ行，与面板ˢ新完ȫ互斥。 */
static uint8_t s_lcd_dir = 0;
static uint16_t s_lcd_soak_sec = 30;

static void lcd_soak_wrapper(void)
{
    LcdTest_RunSoak(s_lcd_soak_sec);
}

static void lcd_test_clear(void)
{
    BSP_LCD_Clear(BSP_LCD_COLOR_BLACK);
}

static void lcd_test_bench(void)
{
    BSP_LCD_Bench();
}

static void lcd_test_dir(void)
{
    BSP_LCD_ScanDir(s_lcd_dir);
    /* 重画方向测试：四边ɫ块边框（红上/绿下/蓝左/黄右）+ 中心ʮ字 */
    uint16_t w = BSP_LCD_GetWidth(), h = BSP_LCD_GetHeight();
    BSP_LCD_Clear(BSP_LCD_COLOR_BLACK);
    BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), 9, BSP_LCD_COLOR_RED);
    BSP_LCD_Fill(0, (uint16_t)(h - 10), (uint16_t)(w - 1),
                 (uint16_t)(h - 1), BSP_LCD_COLOR_GREEN);
    BSP_LCD_Fill(0, 0, 9, (uint16_t)(h - 1), BSP_LCD_COLOR_BLUE);
    BSP_LCD_Fill((uint16_t)(w - 10), 0, (uint16_t)(w - 1),
                 (uint16_t)(h - 1), BSP_LCD_COLOR_YELLOW);
    BSP_LCD_Fill((uint16_t)(w / 2 - 2), (uint16_t)(h / 2 - 40),
                 (uint16_t)(w / 2 + 2), (uint16_t)(h / 2 + 40),
                 BSP_LCD_COLOR_WHITE);
    BSP_LCD_Fill((uint16_t)(w / 2 - 40), (uint16_t)(h / 2 - 2),
                 (uint16_t)(w / 2 + 40), (uint16_t)(h / 2 + 2),
                 BSP_LCD_COLOR_WHITE);
}

static void lcd_test_pattern(void)
{
    static const uint16_t bars[] = {
        BSP_LCD_COLOR_RED, BSP_LCD_COLOR_GREEN, BSP_LCD_COLOR_BLUE,
        BSP_LCD_COLOR_YELLOW, BSP_LCD_COLOR_CYAN, BSP_LCD_COLOR_MAGENTA,
        BSP_LCD_COLOR_WHITE, 0xFBE0
    };
    /* 先清屏避免与既有显ʾ重合 */
    BSP_LCD_Clear(BSP_LCD_COLOR_BLACK);
    uint16_t w = BSP_LCD_GetWidth();
    uint16_t h = BSP_LCD_GetHeight();
    /* 上半屏彩条 */
    for (int i = 0; i < 8; i++) {
        BSP_LCD_Fill((uint16_t)(i * w / 8), 0,
                     (uint16_t)((i + 1) * w / 8 - 1),
                     (uint16_t)(h / 2 - 1), bars[i]);
    }
    BSP_LCD_Fill(0, h / 2, (uint16_t)(w - 1), (uint16_t)(h - 1),
                 BSP_LCD_COLOR_BLACK);
    BSP_LCD_ShowString(8, (uint16_t)(h / 2 + 8), "D00 LCD TEST",
                       BSP_LCD_COLOR_WHITE, BSP_LCD_FONT_24);
    BSP_LCD_ShowString(8, (uint16_t)(h / 2 + 40),
                       "abcdefghijklmnopqrstuvwxyz",
                       BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, (uint16_t)(h / 2 + 64), "0123456789",
                       BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, (uint16_t)(h / 2 + 88),
                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                       BSP_LCD_COLOR_CYAN, BSP_LCD_FONT_16);
}

static void cmd_lcd(const char *args)
{
    if (args == NULL || strcmp(args, "info") == 0) {
        LOG_Printf("LCD: id=0x%04X, %ux%u\r\n",
                   BSP_LCD_GetId(), BSP_LCD_GetWidth(), BSP_LCD_GetHeight());
        return;
    }
    if (strncmp(args, "page", 4) == 0) {
        int pg = atoi(args + 4);
        if (pg >= 0 && pg <= 5) {
            LcdUI_ShowPage((uint8_t)pg);
            LOG_Printf("LCD: page %d\r\n", pg);
        } else {
            LOG_Printf("Usage: lcd page <0-5> (HOME/SYSTEM/BUS/NET/TOUCH/IMU)\r\n");
        }
        return;
    }
    if (strcmp(args, "clear") == 0) {
        LcdUI_EnterTest();
        LcdUI_RunTest(lcd_test_clear);
        LcdUI_ExitTest();   /* 重绘面板恢复干净显ʾ */
        LOG_Printf("LCD: cleared\r\n");
        return;
    }
    if (strcmp(args, "bl") == 0) {
        BSP_LCD_Backlight(1);
        LOG_Printf("LCD: backlight on\r\n");
        return;
    }
    if (strncmp(args, "bl ", 3) == 0) {
        BSP_LCD_Backlight((uint8_t)(atoi(args + 3) ? 1 : 0));
        LOG_Printf("LCD: backlight %s\r\n", atoi(args + 3) ? "on" : "off");
        return;
    }
    if (strcmp(args, "bench") == 0) {
        LcdUI_EnterTest();
        LcdUI_RunTest(lcd_test_bench);
        /* 保持测试画面供观察；按键恢复 HOME */
        return;
    }
    if (strncmp(args, "dir", 3) == 0) {
        int d = atoi(args + 3);
        if (d < 0 || d > 7) {
            LOG_Printf("Usage: lcd dir <0-7>\r\n");
            return;
        }
        s_lcd_dir = (uint8_t)d;
        LcdUI_EnterTest();
        LcdUI_RunTest(lcd_test_dir);
        LOG_Printf("LCD: scan dir=%d\r\n", d);
        return;
    }
    if (strcmp(args, "test") == 0) {
        LcdUI_EnterTest();
        LcdUI_RunTest(lcd_test_pattern);
        LOG_Printf("LCD: test pattern drawn\r\n");
        return;
    }
        if (strcmp(args, "selftest") == 0) {
        LcdUI_EnterTest();
        LcdUI_RunTest(LcdTest_RunSelfTest);
        LcdUI_ExitTest();
        return;
    }
    if (strncmp(args, "soak", 4) == 0) {
        int sec = atoi(args + 4);
        if (sec < 1 || sec > 3600) sec = 30;
        s_lcd_soak_sec = (uint16_t)sec;
        LcdUI_EnterTest();
        LcdUI_RunTest(lcd_soak_wrapper);
        LcdUI_ExitTest();
        return;
    }
    if (strncmp(args, "stress", 6) == 0) {
        int n = atoi(args + 6);
        if (n < 1 || n > 1000) n = 50;
        LcdTest_RunStress((uint16_t)n);
        return;
    }
    LOG_Printf("Usage: lcd <info|test|clear|bench|dir <0-7>|selftest|soak <sec>|stress <n>|bl <0|1>>\r\n");
}
#if CRASH_INJECT_ENABLE
/* ================== 崩溃注入（仅调试构建，用于验证纠错系统） ==================
 * 用法：
 *   crash bus     -> 写非法地址触发 BusFault
 *   crash undef   -> 跳转非法指令触发 UsageFault/HardFault
 *   crash stack   -> 无限递归触发 FreeRTOS 栈溢出检测
 *   crash assert  -> 直接调用 ERR_HandleAssert 模拟 RTOS 断言失败 */
__attribute__((noinline)) static void crash_bus(void)
{
    *(volatile uint32_t *)0xDEADBEEFu = 0x55u;
}

__attribute__((noinline)) static void crash_undef(void)
{
    ((void (*)(void))0xFFFFFFFFu)();
}

__attribute__((noinline)) static void crash_stack(int depth)
{
    volatile uint8_t pad[128];
    pad[0] = (uint8_t)0xAA;
    (void)pad;
    if (depth > 0) {
        taskYIELD();          /* 让调度器在递归间隙做栈溢出检查*/
        crash_stack(depth - 1);
    }
}


static void cmd_crash(const char *args)
{
    if (args == NULL) {
        LOG_Printf("Usage: crash <bus|undef|stack|assert>\r\n");
        return;
    }
    LOG_Printf("Injecting crash: %s\r\n", args);
    if (strcmp(args, "bus") == 0) {
        crash_bus();
    } else if (strcmp(args, "undef") == 0) {
        crash_undef();
    } else if (strcmp(args, "stack") == 0) {
        crash_stack(200);     /* 128B×200 远超 2KB 任务栈，触发溢出检测*/
    } else if (strcmp(args, "assert") == 0) {
        ERR_HandleAssert(0xBADFu);
    } else if (strcmp(args, "irq") == 0) {
        /* 使能并置位一个未实现处理器的中断，触发 Default_Handler 诊断 */
        NVIC_EnableIRQ(TIM4_IRQn);
        NVIC_SetPendingIRQ(TIM4_IRQn);
    } else if (strcmp(args, "unhandled") == 0) {
        /* 直接调用未处理中断诊断（绕过中断路径，用于定位） */
        ERR_HandleUnhandledIRQ(46u);
    } else {
        LOG_Printf("Unknown crash type\r\n");
    }
}
#endif

/* ================== 用户存储（EEPROM usr_store 自检/调试） ================== */
static void cmd_usr(const char *args)
{
    if (args == NULL || *args == '\0' || strncmp(args, "info", 4) == 0) {
        uint32_t used = 0, free = 0;
        UsrStore_Info(&used, &free);
        LOG_Printf("USR: valid=%d keys=%lu used=%luB free=%luB eeprom=%s\r\n",
                   UsrStore_Valid(), (unsigned long)UsrStore_Count(),
                   (unsigned long)used, (unsigned long)free,
                   BSP_EEPROM_Probe() == 0 ? "OK" : "MISS");
        return;
    }
    if (strncmp(args, "scan", 4) == 0) {
        LOG_Printf("USR log layout (offset|key|len|crc):\r\n");
        uint16_t off = 0;
        while (off < 256u) {
            uint8_t hdr[5];
            if (BSP_EEPROM_Read(off, hdr, 5) != 0 || hdr[0] != 0xA5u) {
                break;
            }
            uint16_t dlen = (hdr[2] == 0xFFu) ? 0u : hdr[2];
            LOG_Printf("  %04X: key=%u len=%u crc=%02X%02X%s\r\n",
                       (unsigned)off, (unsigned)hdr[1], (unsigned)hdr[2],
                       (unsigned)hdr[3], (unsigned)hdr[4],
                       (hdr[2] == 0xFFu) ? " (tomb)" : "");
            off = (uint16_t)(off + 5u + dlen);
        }
        LOG_Printf("  log end @0x%04X, free=%uB\r\n",
                   (unsigned)off, (unsigned)(256u - off));
        return;
    }
    if (strncmp(args, "get", 3) == 0) {
        int key = atoi(args + 3);
        uint8_t buf[USR_DATA_MAX];
        int n = UsrStore_Get((uint8_t)key, buf, sizeof(buf));
        if (n < 0) {
            LOG_Printf("USR: key %d not found\r\n", key);
        } else {
            LOG_Printf("USR: key %d len=%d data=", key, n);
            for (int i = 0; i < n; i++) {
                LOG_Printf("%02X", buf[i]);
            }
            LOG_Printf("\r\n");
        }
        return;
    }
    if (strncmp(args, "set", 3) == 0) {
        const char *p = args + 3;
        int key = atoi(p);
        while (*p == ' ' || *p == '\t') p++;
        while ((*p >= '0' && *p <= '9') || *p == '-') p++;
        while (*p == ' ' || *p == '\t') p++;
        uint8_t buf[USR_DATA_MAX];
        uint16_t n = 0;
        while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            unsigned int v = 0;
            sscanf(p, "%2x", &v);
            if (n < sizeof(buf)) {
                buf[n++] = (uint8_t)v;
            }
            p += 2;
        }
        int r = UsrStore_Set((uint8_t)key, buf, n);
        LOG_Printf("USR: set key %d len=%u -> %d\r\n", key, (unsigned)n, r);
        return;
    }
    if (strncmp(args, "erase", 5) == 0) {
        int key = atoi(args + 5);
        LOG_Printf("USR: erase key %d -> %d\r\n",
                   key, UsrStore_Erase((uint8_t)key));
        return;
    }
    if (strncmp(args, "reset", 5) == 0) {
        LOG_Printf("USR: reset -> %d\r\n", UsrStore_Reset());
        return;
    }
    LOG_Printf("Usage: usr <info|scan|get <key>|set <key> <hex>|erase <key>|reset>\r\n");
}

/* ================== 事件总线极限负载测试 ==================
 * 用法：
 *   eb_stress <count> [payload] [burst|steady]
 *   - burst ：挂起 eventBusTask 后连发，测纯发布速率与缓冲池上限；
 *   - steady：不挂起连发，测系统稳态吞吐（消费者实时消化）与丢包拐点。
 * payload 为每条消息字节数（≤ EVENT_BUS_MSG_MAX_PAYLOAD）。*/
extern TaskHandle_t eventBusTaskHandle;

static volatile uint32_t g_eb_processed = 0;
static uint8_t           g_eb_subscribed = 0;

static void eb_stress_handler(const message_t *msg)
{
    (void)msg;
    g_eb_processed++;
}

static void cmd_eb_stress(const char *args)
{
    uint32_t count = 10000;
    uint16_t payload = 0;
    char mode[16] = "burst";
    if (args && *args) {
        unsigned long c = 0;
        unsigned int p = 0;
        int n = sscanf(args, "%lu %u %15s", &c, &p, mode);
        if (n >= 1) count = (uint32_t)c;
        if (n >= 2) payload = (uint16_t)p;
    }
    if (count == 0) count = 1;
    if (payload > EVENT_BUS_MSG_MAX_PAYLOAD) payload = EVENT_BUS_MSG_MAX_PAYLOAD;
    int steady = (strcmp(mode, "steady") == 0);

    if (!g_eb_subscribed) {
        EventBus_Subscribe(MSG_EB_STRESS, eb_stress_handler);
        g_eb_subscribed = 1;
    }

    /* DWT 周期计数（@168MHz）*/
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t lost0 = EventBus_GetLostCount();
    uint32_t proc0 = g_eb_processed;

    if (!steady) {
        vTaskSuspend(eventBusTaskHandle);
    }
    DWT->CYCCNT = 0;
    uint32_t pub_ok = 0, pub_lost = 0;
    for (uint32_t i = 0; i < count; i++) {
        message_t *msg = NULL;
        if (EventBus_AllocMsg(MODULE_SHELL, MSG_EB_STRESS,
                              payload, &msg) == 0) {
            if (payload) memset(msg->payload, 0xA5, payload);
            if (EventBus_Publish(msg) == 0) {
                pub_ok++;
            } else {
                pub_lost++;
            }
        } else {
            pub_lost++;
        }
    }
    uint32_t pub_cycles = DWT->CYCCNT;
    if (!steady) {
        vTaskResume(eventBusTaskHandle);
    }

    /* 等待消费者消化完成（最大 5s）*/
    uint32_t wait_ms = 0;
    while (g_eb_processed - proc0 < pub_ok && wait_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(2));
        wait_ms += 2;
    }
    uint32_t proc_done = g_eb_processed - proc0;
    uint32_t lost_delta = EventBus_GetLostCount() - lost0;

    /* 速率（整数计算，@168MHz）*/
    uint32_t cpmsg = pub_cycles / count;              /* cycles/msg（含失败路径）*/
    uint32_t pub_rate = cpmsg ? (168000000u / cpmsg) : 0;   /* 发布 msg/s */
    uint32_t total_us = pub_cycles / 168u + wait_ms * 1000u; /* 总耗时（µs锛?*/
    uint64_t sys_rate64 = total_us ? ((uint64_t)proc_done * 1000000u / total_us) : 0;
    uint32_t sys_rate = (uint32_t)sys_rate64;

    LOG_Printf("EB STRESS: count=%lu payload=%u mode=%s\r\n",
               (unsigned long)count, (unsigned)payload, mode);
    LOG_Printf("  published OK: %lu, lost: %lu\r\n",
               (unsigned long)pub_ok, (unsigned long)pub_lost);
    LOG_Printf("  publish: %lu cycles (%lu cpmsg, %lu msg/s)\r\n",
               (unsigned long)pub_cycles, (unsigned long)cpmsg,
               (unsigned long)pub_rate);
    LOG_Printf("  processed: %lu / %lu after %lu ms\r\n",
               (unsigned long)proc_done, (unsigned long)pub_ok,
               (unsigned long)wait_ms);
    LOG_Printf("  system throughput: %lu msg/s (%lu us)\r\n",
               (unsigned long)sys_rate, (unsigned long)total_us);
    LOG_Printf("  total lost delta: %lu, pool free: %lu, queue: %lu\r\n",
               (unsigned long)lost_delta,
               (unsigned long)EventBus_GetPoolFreeCount(),
               (unsigned long)EventBus_GetQueueCount());
}

void CmdCatalog_Register(void)
{
    Cmd_Register(cmd_table, CMD_COUNT);
}
