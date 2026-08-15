/* ================================================================
 * cmd_catalog —— 命令目录：全部 cmd_* 实现（传输无关）
 *
 * 架构位置：APP 应用层；命令是应用级接线，被 Shell(UART)/TCP 控制台共用
 * 核心流程：命令只声明 transport 掩码 -> 输出走 LOG_Printf 由核心路由
 * 关键约束：新增命令 = 加 cmd_xxx 实现 + cmd_table 一行
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
#include "gui_app.h"
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
#include "ota_agent.h"
#include "ota_transport.h"
#include "ota_tcp_svc.h"
#include "ota_http_svc.h"
#include "cmd_shell.h"
#include "usr_store.h"
#include "bsp_eeprom.h"
#include "bsp_mpu6050.h"
#include "bsp_i2c.h"
#include "i2c.h"
#include "bsp_can.h"
#include "bsp_power.h"
#include "bsp_w25q128.h"
#include "ext_store.h"
#include "can_proto.h"
#include "pinout.h"
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
    if (args != NULL && strncmp(args, "gw ", 3) == 0) {
        const char *gw = args + 3;
        while (*gw == ' ' || *gw == '\t') gw++;
        if (*gw == '\0') {
            LOG_Printf("Usage: net gw <a.b.c.d>\r\n");
            return;
        }
        if (EthApp_SetStaticGWPersist(gw) == 0) {
            LOG_Printf("Gateway set to %s (saved to EEPROM)\r\n", gw);
        } else {
            LOG_Printf("Invalid gateway: %s\r\n", gw);
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
static void cmd_gui(const char *args);
static void cmd_touch(const char *args);
static void cmd_beep(const char *args);
static void cmd_power(const char *args);
static void cmd_mpu(const char *args);
static void cmd_can(const char *args);
static void cmd_w25q(const char *args);
static void cmd_store(const char *args);
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
        uint16_t port = 1123u;   /* 与 SNTP_PORT 保持一致（本地 NTP 服务器） */
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

/* 命令表（47 条）——按域导航（新增命令追加到对应域注释下）：
 *   系统:  help/info/reset/led/taskstats/ver/echo/sysmon/eb_stress
 *   OTA :  ota/ota_rbtest
 *   LA  :  la_start/la_stop/la_first/la_trig/la_dma_start/la_dma_stop
 *          la_dump/la_info/la_state/la_peek
 *   信号:  sg_uart_start/sg_uart_stop/sg_uart_hex/sg_spi_start/sg_spi_stop
 *          sg_i2c_start/sg_i2c_stop/sg_i2c_complex
 *   网络:  tcp/net/icmp/dhcp/dns/sntp/mqtt/http/stream
 *   存储:  usr/store/w25q
 *   硬件:  lcd/touch/beep/power/mpu/can
 *   诊断:  crash（仅 CRASH_INJECT_ENABLE）
 * 设计权衡：命令实现集中于此（组合根模式，见 ARCHITECTURE.md §1）。
 * 当单域命令膨胀到 ~20 条或本文件超 3000 行时，按域拆分为
 * cmd_la.c、cmd_net.c、cmd_storage.c 再在此聚合注册。 */
static const cmd_entry_t cmd_table[] = {
    {"help",         "Show command help", CMD_TRANSPORT_ALL, cmd_help},
    {"info",         "System info (version/kernel/tasks)", CMD_TRANSPORT_ALL, cmd_info},
    {"reset",        "Software reset", CMD_TRANSPORT_ALL, cmd_reset},
    {"led",          "LED control (on/off/toggle/blink)", CMD_TRANSPORT_ALL, cmd_led},
    {"taskstats",    "Task list & stack usage", CMD_TRANSPORT_ALL, cmd_taskstats},
    {"ota",          "OTA <enter BOOT|status|abort|tcp|http <url>>", CMD_TRANSPORT_ALL, cmd_ota},
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
    {"lcd",          "LCD <info|bl <0|1>> (LVGL master)", CMD_TRANSPORT_ALL, cmd_lcd},
    {"gui",          "GUI bench (LVGL performance)", CMD_TRANSPORT_ALL, cmd_gui},
    {"touch",        "Touch <info|cal|test>", CMD_TRANSPORT_ALL, cmd_touch},
    {"usr",          "User storage <info|scan|get|set|erase|reset>", CMD_TRANSPORT_ALL, cmd_usr},
    {"beep",         "Buzzer beep <ms|test|off>", CMD_TRANSPORT_ALL, cmd_beep},
    {"power",        "Power <on|off|info> (STOP tickless)", CMD_TRANSPORT_ALL, cmd_power},
    {"mpu",          "IMU MPU6050 <info|test|cal>", CMD_TRANSPORT_ALL, cmd_mpu},
    {"can",          "CAN1 <status|reset|loop <on|off|silent>|test <n>|send <id> <hex>>", CMD_TRANSPORT_ALL, cmd_can},
    {"w25q",         "W25Q128 <id|read <addr> <len>|write <addr> <hex>|erase|erase32|erase64 <addr>|erasc|sr|bench>", CMD_TRANSPORT_ALL, cmd_w25q},
    {"store",        "ExtFlash store <info|probe|erase|badclear <part>|read <part> <off> <len>|write <part> <off> <hex>|ws|rs <part> <slot> <stride>|bench>", CMD_TRANSPORT_ALL, cmd_store},
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

/* 精确匹配子命令（后随空白或结尾，避免 "statusx" 误匹配 "status"） */
static int arg_match(const char *s, const char *w)
{
    size_t n = strlen(w);
    return (strncmp(s, w, n) == 0 &&
            (s[n] == '\0' || s[n] == ' ' || s[n] == '\t'));
}

static void cmd_ota(const char *args)
{
    if (args == NULL || *args == '\0') {
        /* 原行为：进入 BOOT 升级模式（串口 HOSTLINK 流程） */
        LOG_Printf("OTA command received, publishing event...\r\n");
        MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_OTA_START);
        return;
    }
    if (arg_match(args, "status") || arg_match(args, "info")) {
        uint8_t state;
        uint32_t rx = 0, total = 0;
        Ota_Status(&state, &rx, &total);
        static const char *stn[] = {"IDLE", "RECEIVING", "DONE"};
        LOG_Printf("OTA: state=%s %lu/%lu bytes\r\n",
                   (state < 3u) ? stn[state] : "?",
                   (unsigned long)rx, (unsigned long)total);
        LOG_Printf("OTA transports:\r\n");
        for (uint8_t i = 0; i < OtaMgr_Count(); i++) {
            const ota_transport_t *t = OtaMgr_Get(i);
            if (t != NULL) {
                LOG_Printf("  [%u] %-8s %-24s %s\r\n",
                           (unsigned)t->id, t->name, t->desc,
                           t->available ? "ready" : "reserved");
            }
        }
        return;
    }
    if (arg_match(args, "abort")) {
        LOG_Printf("OTA: abort -> %u\r\n", (unsigned)Ota_Reset());
        return;
    }
    if (arg_match(args, "tcp")) {
        LOG_Printf("OTA-TCP: server :%u sessions=%lu\r\n",
                   (unsigned)OTA_TCP_PORT,
                   (unsigned long)OtaTcpSvc_GetSessions());
        return;
    }
    if (arg_match(args, "http")) {
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') p++;
        /* 兼容完整 URL：跳过 http:// 或 https:// 前缀 */
        if (strncmp(p, "http://", 7) == 0) {
            p += 7;
        } else if (strncmp(p, "https://", 8) == 0) {
            p += 8;
        }
        if (*p == '\0') {
            LOG_Printf("Usage: ota http <ip[:port]>/<path>\r\n");
            return;
        }
        char url[96];
        int ul = 0;
        while (*p != '\0' && ul < 95) {
            url[ul++] = *p++;
        }
        url[ul] = '\0';
        /* 解析 ip[:port]/path */
        char host[48];
        uint16_t port = 80u;
        const char *hp = url;
        const char *slash = strchr(hp, '/');
        if (slash == NULL) {
            LOG_Printf("Usage: ota http <ip[:port]>/<path>\r\n");
            return;
        }
        int hl = (int)(slash - hp);
        if (hl <= 0 || hl >= (int)sizeof(host)) {
            LOG_Printf("OTA-HTTP: bad url %s\r\n", url);
            return;
        }
        memcpy(host, hp, hl);
        host[hl] = '\0';
        const char *colon = strchr(host, ':');
        if (colon != NULL) {
            *((char *)colon) = '\0';
            port = (uint16_t)atoi(colon + 1);
        }
        int r = OtaHttp_Download(host, port, slash);
        LOG_Printf("OTA-HTTP: download -> %d\r\n", r);
        return;
    }
    LOG_Printf("Usage: ota <enter BOOT|status|abort|tcp|http <ip[:port]>/<path>>\r\n");
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
        char b1[16], b2[16], b3[16], b4[16], b5[16], b6[16], b7[16];
        ImuSvc_FormatFixed(s->roll, 2, b1);
        ImuSvc_FormatFixed(s->pitch, 2, b2);
        ImuSvc_FormatFixed(s->yaw, 2, b3);
        LOG_Printf("MPU: ready=%u samples=%lu faults=%lu\r\n",
                   (unsigned)s->ready, (unsigned long)s->sample_count,
                   (unsigned long)s->fault_count);
        LOG_Printf("MPU: R=%s P=%s Y=%s deg\r\n",
                   b1, b2, b3);
        ImuSvc_FormatFixed(s->ax, 3, b1);
        ImuSvc_FormatFixed(s->ay, 3, b2);
        ImuSvc_FormatFixed(s->az, 3, b3);
        ImuSvc_FormatFixed(s->gx, 2, b4);
        ImuSvc_FormatFixed(s->gy, 2, b5);
        ImuSvc_FormatFixed(s->gz, 2, b6);
        ImuSvc_FormatFixed(s->temp, 1, b7);
        LOG_Printf("MPU: A=(%s,%s,%s)g G=(%s,%s,%s)dps T=%sC\r\n",
                   b1, b2, b3, b4, b5, b6, b7);
        return;
    }
    if (strcmp(args, "test") == 0) {
        LOG_Printf("MPU: streaming R/P/Y and G for 2s...\r\n");
        for (int i = 0; i < 20; i++) {
            char b1[16], b2[16], b3[16], b4[16], b5[16], b6[16];
            ImuSvc_FormatFixed(s->roll, 2, b1);
            ImuSvc_FormatFixed(s->pitch, 2, b2);
            ImuSvc_FormatFixed(s->yaw, 2, b3);
            ImuSvc_FormatFixed(s->gx, 1, b4);
            ImuSvc_FormatFixed(s->gy, 1, b5);
            ImuSvc_FormatFixed(s->gz, 1, b6);
            LOG_Printf("MPU: %8s %8s %8s | %6s %6s %6s\r\n",
                       b1, b2, b3, b4, b5, b6);
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
/* ================== CAN 命令 ==================
 * 用法：can <status|reset|loop <on|off|silent>|test <n>|send <id> <hex>> */
static void cmd_can(const char *args)
{
    if (args == NULL || strcmp(args, "status") == 0 || strcmp(args, "stats") == 0) {
        bsp_can_stats_t st;
        BSP_CAN_GetStats(&st);
        uint8_t tec = 0, rec = 0;
        BSP_CAN_GetErrorCounters(&tec, &rec);
        const char *mode = "normal";
        if (BSP_CAN_GetMode() == BSP_CAN_MODE_LOOPBACK) {
            mode = "loopback";
        } else if (BSP_CAN_GetMode() == BSP_CAN_MODE_SILENT_LOOPBACK) {
            mode = "silent";
        }
        LOG_Printf("CAN1: %s @1Mbps mode=%s\r\n",
                   BSP_CAN_IsActive() ? "active" : "off", mode);
        LOG_Printf("  TX ok=%lu err=%lu | RX ok=%lu other=%lu drop=%lu ovr=%lu\r\n",
                   (unsigned long)st.tx_ok, (unsigned long)st.tx_err,
                   (unsigned long)st.rx_ok, (unsigned long)st.rx_other,
                   (unsigned long)st.rx_drop, (unsigned long)st.rx_overrun);
        LOG_Printf("  ERR ewg=%lu epv=%lu boff=%lu last=0x%08lX TEC=%u REC=%u\r\n",
                   (unsigned long)st.err_warning, (unsigned long)st.err_passive,
                   (unsigned long)st.err_busoff, (unsigned long)st.last_error,
                   (unsigned)tec, (unsigned)rec);
        LOG_Printf("  BUS load ~%lu.%lu%%\r\n",
                   (unsigned long)(st.bus_load_permille / 10u),
                   (unsigned long)(st.bus_load_permille % 10u));
        return;
    }
    if (strcmp(args, "reset") == 0) {
        BSP_CAN_ResetStats();
        LOG_Printf("CAN: stats cleared\r\n");
        return;
    }
    if (strncmp(args, "loop", 4) == 0) {
        if (strstr(args, "silent") != NULL) {
            BSP_CAN_SetMode(BSP_CAN_MODE_SILENT_LOOPBACK);
        } else if (strstr(args, "off") != NULL) {
            BSP_CAN_SetMode(BSP_CAN_MODE_NORMAL);
        } else {
            BSP_CAN_SetMode(BSP_CAN_MODE_LOOPBACK);
        }
        LOG_Printf("CAN: mode switched (shell link off in loopback, use UART)\r\n");
        return;
    }
    if (strncmp(args, "test", 4) == 0) {
        int n = atoi(args + 4);
        if (n < 1 || n > 10000) {
            n = 100;
        }
        LOG_Printf("CAN: burst test %d frames (ID 0x%03X)\r\n", n, CAN_TEST_ID);
        for (int i = 0; i < n; i++) {
            uint8_t f[8];
            f[0] = (uint8_t)i;
            f[1] = (uint8_t)(i >> 8);
            f[2] = 0xAA;
            f[3] = 0x55;
            f[4] = (uint8_t)~i;
            f[5] = 0x5A;
            f[6] = 0xA5;
            f[7] = 0x00;
            if (BSP_CAN_Send(CAN_TEST_ID, f, 8) != 0) {
                LOG_Printf("CAN: TX queue full at frame %d\r\n", i);
                break;
            }
        }
        LOG_Printf("CAN: burst sent\r\n");
        return;
    }
    if (strncmp(args, "send", 4) == 0) {
        unsigned int id = 0;
        uint8_t buf[8];
        int n = 0;
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (sscanf(p, "%x", &id) != 1 || id > 0x7FFu) {
            LOG_Printf("Usage: can send <id(hex)> <hex bytes...>\r\n");
            return;
        }
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        while (n < 8) {
            unsigned int b = 0;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (sscanf(p, "%2x", &b) != 1) {
                break;
            }
            buf[n++] = (uint8_t)b;
            p += 2;
        }
        if (BSP_CAN_Send(id, buf, (uint8_t)n) == 0) {
            LOG_Printf("CAN: sent ID=0x%03X DLC=%d\r\n", id, n);
        } else {
            LOG_Printf("CAN: TX queue full\r\n");
        }
        return;
    }
    LOG_Printf("Usage: can <status|reset|loop <on|off|silent>|test <n>|send <id> <hex>>\r\n");
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

  /* ================== 低功耗命令 ==================
   * 用法：power <on|off|info>
   * on 开启 STOP Tickless（空闲 >2s 时休眠；CAN/ETH 数据暂停）；
   * off 回到常驻模式（默认，工业安全）；WFI 空闲钩子始终生效。 */
  static void cmd_power(const char *args)
  {
      if (args == NULL || strcmp(args, "info") == 0) {
          LOG_Printf("PWR: STOP tickless %s (WFI idle always on)\r\n",
                     BSP_Power_IsEnabled() ? "ON" : "OFF");
          return;
      }
      if (strcmp(args, "on") == 0) {
          (void)BSP_Power_Enable();
          return;
      }
      if (strcmp(args, "off") == 0) {
          BSP_Power_Disable();
          return;
      }
      LOG_Printf("Usage: power <on|off|info>\r\n");
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
        LOG_Printf("TOUCH: test moved to LVGL GUI (touch page) - use touch info/cal\r\n");
        return;
    }
    LOG_Printf("Usage: touch <info|cal|nudge <dx> <dy>|test>\r\n");
}
/* ================== LCD 命令（LVGL 接管显示） ==================
 * 用法：lcd <info|bl <0|1>>
 * LVGL 作为唯一显示层后，原自绘页面/测试/老化命令全部移除，
 * 渲染由 GuiApp 任务统一负责；本命令仅保留信息查询与背光控制。 */
static void cmd_lcd(const char *args)
{
    if (args == NULL || strcmp(args, "info") == 0) {
        LOG_Printf("LCD: id=0x%04X, %ux%u, LVGL GUI master\r\n",
                   BSP_LCD_GetId(), BSP_LCD_GetWidth(), BSP_LCD_GetHeight());
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
    if (strcmp(args, "selftest") == 0) {
        BSP_LCD_SelfTest();
        return;
    }
    LOG_Printf("Usage: lcd <info|bl <0|1>|selftest>\r\n");
}

static void cmd_gui(const char *args)
{
    if (args != NULL && strcmp(args, "bench") == 0) {
        LOG_Printf("GUI: bench requested (runs in GUI task)...\r\n");
        GuiApp_Bench();
        return;
    }
    LOG_Printf("Usage: gui <bench>\r\n");
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

/* ================== W25Q128 SPI Flash ================== */
static void cmd_w25q(const char *args)
{
    char sub[16] = {0};
    unsigned long addr = 0, len = 0;
    int n = sscanf(args, "%15s %lx %lu", sub, &addr, &len);
    if (n < 1) {
        LOG_Printf("Usage: w25q <id|read <addr> <len>|write <addr> <hex>|"
                   "erase|erase32|erase64 <addr>|erasc|sr|bench>\r\n");
        return;
    }

    if (strcmp(sub, "id") == 0 || strcmp(sub, "info") == 0) {
        bsp_w25q_info_t info;
        BSP_W25Q128_Info(&info);
        int rc = BSP_W25Q128_Probe();
        LOG_Printf("W25Q probe: %d (0=OK)\r\n", rc);
        LOG_Printf("  JEDEC: %02X %02X %02X  size=%luKB  page=%lu  sector=%lu\r\n",
                   (unsigned)info.jedec[0], (unsigned)info.jedec[1],
                   (unsigned)info.jedec[2],
                   (unsigned long)(info.size / 1024u),
                   (unsigned long)info.page, (unsigned long)info.sector);
        return;
    }

    if (strcmp(sub, "read") == 0 && n >= 3) {
        if (len == 0 || len > 1024u) {
            LOG_Printf("len 1..1024\r\n");
            return;
        }
        uint8_t buf[16];
        uint32_t off = 0;
        int rc_all = BSP_W25Q_OK;
        while (off < len && rc_all == BSP_W25Q_OK) {
            uint32_t chunk = (len - off > sizeof(buf)) ? sizeof(buf) : (len - off);
            rc_all = BSP_W25Q128_Read((uint32_t)addr + off, buf, chunk);
            if (rc_all != BSP_W25Q_OK) {
                break;
            }
            LOG_Printf("%08lX: ", (unsigned long)(addr + off));
            for (uint32_t i = 0; i < chunk; i++) {
                LOG_Printf("%02X ", (unsigned)buf[i]);
            }
            LOG_Printf("\r\n");
            off += chunk;
        }
        LOG_Printf("read rc=%d (%lu bytes)\r\n", rc_all, (unsigned long)off);
        return;
    }

    if (strcmp(sub, "write") == 0 && n >= 3) {
        /* 从 args 中提取命令名与地址后的 hex 字节串 */
        const char *p = args;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        uint8_t buf[64];
        uint32_t cnt = 0;
        while (*p && cnt < sizeof(buf)) {
            unsigned int v = 0;
            if (sscanf(p, "%2x", &v) != 1) {
                break;
            }
            buf[cnt++] = (uint8_t)v;
            p += 2;
            while (*p == ' ') p++;
        }
        if (cnt == 0) {
            LOG_Printf("no hex data\r\n");
            return;
        }
        int rc = BSP_W25Q128_Write((uint32_t)addr, buf, cnt);
        LOG_Printf("write %lu bytes @0x%lX rc=%d\r\n",
                   (unsigned long)cnt, (unsigned long)addr, rc);
        return;
    }

    if (strcmp(sub, "erase") == 0 || strcmp(sub, "erase32") == 0 ||
        strcmp(sub, "erase64") == 0) {
        uint32_t t0 = HAL_GetTick();
        int rc;
        if (strcmp(sub, "erase32") == 0) {
            rc = BSP_W25Q128_EraseBlock32((uint32_t)addr);
        } else if (strcmp(sub, "erase64") == 0) {
            rc = BSP_W25Q128_EraseBlock64((uint32_t)addr);
        } else {
            rc = BSP_W25Q128_EraseSector((uint32_t)addr);
        }
        LOG_Printf("%s @0x%lX rc=%d in %lu ms\r\n", sub,
                   (unsigned long)addr, rc,
                   (unsigned long)(HAL_GetTick() - t0));
        return;
    }

    if (strcmp(sub, "erasc") == 0) {
        uint32_t t0 = HAL_GetTick();
        int rc = BSP_W25Q128_EraseChip();
        LOG_Printf("chip erase rc=%d in %lu ms\r\n", rc,
                   (unsigned long)(HAL_GetTick() - t0));
        return;
    }

    if (strcmp(sub, "sr") == 0) {
        int sr = BSP_W25Q128_Status();
        LOG_Printf("SR1=0x%02X (busy=%u wel=%u)\r\n",
                   (unsigned)(sr & 0xFFu), (unsigned)((sr >> 0) & 1u),
                   (unsigned)((sr >> 1) & 1u));
        return;
    }

    if (strcmp(sub, "dbg") == 0) {
        /* 简洁诊断：一次 DMA 读 + SPI/DMA 寄存器快照（不改变 Flash 状态） */
        uint8_t buf[16];
        int rc = BSP_W25Q128_Read(0u, buf, sizeof(buf));
        LOG_Printf("read rc=%d\r\n", rc);
        LOG_Printf("SPI1 CR1=0x%08X SR=0x%02X | DMA2 S0NDTR=%lu LISR=0x%08X\r\n",
                   (unsigned)SPI1->CR1, (unsigned)(SPI1->SR & 0xFFu),
                   (unsigned long)DMA2_Stream0->NDTR, (unsigned)DMA2->LISR);
        return;
    }

    if (strcmp(sub, "bench") == 0) {
        static uint8_t rbuf[512];   /* 静态：避免命令层栈压力 */
        static uint8_t pat[128];
        static uint8_t chk[128];
        /* 性能测试区：最后 1MB（0xFF0000），与后续分区规划不冲突 */
        const uint32_t base = 0xFF0000u;
        const uint32_t blk = 64u * 1024u;
        for (uint32_t i = 0; i < sizeof(pat); i++) {
            pat[i] = (uint8_t)i;
        }

        /* 1) 读吞吐：1MB DMA Fast Read */
        uint32_t t0 = HAL_GetTick();
        int rc = BSP_W25Q_OK;
        for (uint32_t off = 0; off < (1024u * 1024u) && rc == BSP_W25Q_OK;
             off += sizeof(rbuf)) {
            rc = BSP_W25Q128_Read(off, rbuf, sizeof(rbuf));
        }
        uint32_t rd_ms = HAL_GetTick() - t0;
        LOG_Printf("bench read 1MB: rc=%d %lu ms -> %lu KB/s\r\n", rc,
                   (unsigned long)rd_ms,
                   (unsigned long)(rd_ms ? (1024u * 1000u / rd_ms) : 0u));

        /* 2) 擦除 64KB 块 */
        t0 = HAL_GetTick();
        rc = BSP_W25Q128_EraseBlock64(base);
        uint32_t er_ms = HAL_GetTick() - t0;
        LOG_Printf("bench erase 64KB: rc=%d %lu ms\r\n", rc,
                   (unsigned long)er_ms);

        /* 3) 写 64KB（256 页） */
        t0 = HAL_GetTick();
        rc = BSP_W25Q_OK;
        for (uint32_t off = 0; off < blk && rc == BSP_W25Q_OK; off += sizeof(pat)) {
            rc = BSP_W25Q128_Write(base + off, pat, sizeof(pat));
        }
        uint32_t wr_ms = HAL_GetTick() - t0;
        LOG_Printf("bench write 64KB: rc=%d %lu ms -> %lu KB/s\r\n", rc,
                   (unsigned long)wr_ms,
                   (unsigned long)(wr_ms ? (blk * 1000u / wr_ms) : 0u));

        /* 4) 读回校验 */
        t0 = HAL_GetTick();
        int bad = 0;
        for (uint32_t off = 0; off < blk && bad == 0; off += sizeof(chk)) {
            rc = BSP_W25Q128_Read(base + off, chk, sizeof(chk));
            if (rc != BSP_W25Q_OK ||
                memcmp(chk, pat, sizeof(chk)) != 0) {
                bad = 1;
            }
        }
        LOG_Printf("bench verify 64KB: rc=%d bad=%d\r\n", rc, bad);

        /* 5) 恢复：擦回测试区 */
        t0 = HAL_GetTick();
        rc = BSP_W25Q128_EraseBlock64(base);
        LOG_Printf("bench restore: rc=%d in %lu ms\r\n", rc,
                   (unsigned long)(HAL_GetTick() - t0));
        return;
    }

    LOG_Printf("unknown w25q sub: %s\r\n", sub);
}

/* ================== 外部 Flash 存储服务层 ================== */
static int store_part_parse(const char *s)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    for (int i = 0; i < EXT_PART_COUNT; i++) {
        const ext_part_desc_t *p = ExtStore_GetPart((ext_part_id_t)i);
        if (p != NULL && strcmp(s, p->name) == 0) {
            return i;
        }
    }
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end != s && v >= 0 && v < EXT_PART_COUNT) {
        return (int)v;
    }
    return -1;
}

static void cmd_store(const char *args)
{
    char sub[16] = {0};
    char pname[16] = {0};
    unsigned long a = 0, b = 0;
    int n = sscanf(args, "%15s %15s %lx %lx", sub, pname, &a, &b);
    if (n < 1) {
        LOG_Printf("Usage: store <info|probe|erase <part>|read <part> <off> <len>|"
                   "write <part> <off> <hex>|ws|rs <part> <slot> <stride>|bench>\r\n");
        return;
    }

    if (strcmp(sub, "info") == 0) {
        LOG_Printf("ExtFlash partitions:\r\n");
        for (int i = 0; i < EXT_PART_COUNT; i++) {
            const ext_part_desc_t *p = ExtStore_GetPart((ext_part_id_t)i);
            if (p == NULL) {
                continue;
            }
            LOG_Printf("  [%d] %-8s base=0x%06lX size=%luKB flags=0x%lX\r\n",
                       i, p->name, (unsigned long)p->base,
                       (unsigned long)(p->size / 1024u),
                       (unsigned long)p->flags);
        }
        return;
    }

    if (strcmp(sub, "probe") == 0) {
        LOG_Printf("store probe rc=%d\r\n", ExtStore_Probe());
        return;
    }

    if (strcmp(sub, "erase") == 0 && n >= 2) {
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        uint32_t t0 = HAL_GetTick();
        int rc = ExtStore_Erase((ext_part_id_t)id);
        LOG_Printf("erase %s rc=%d in %lu ms\r\n", pname, rc,
                   (unsigned long)(HAL_GetTick() - t0));
        return;
    }

    if (strcmp(sub, "read") == 0 && n >= 4) {
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        if (b == 0 || b > 1024u) {
            LOG_Printf("len 1..1024\r\n");
            return;
        }
        uint8_t buf[16];
        uint32_t off = 0;
        int rc_all = EXT_STORE_OK;
        while (off < b && rc_all == EXT_STORE_OK) {
            uint32_t chunk = (b - off > sizeof(buf)) ? sizeof(buf) : (b - off);
            rc_all = ExtStore_Read((ext_part_id_t)id, (uint32_t)a + off,
                                   buf, chunk);
            if (rc_all != EXT_STORE_OK) {
                break;
            }
            LOG_Printf("%06lX: ", (unsigned long)(a + off));
            for (uint32_t i = 0; i < chunk; i++) {
                LOG_Printf("%02X ", (unsigned)buf[i]);
            }
            LOG_Printf("\r\n");
            off += chunk;
        }
        LOG_Printf("read rc=%d (%lu bytes)\r\n", rc_all, (unsigned long)off);
        return;
    }

    if (strcmp(sub, "write") == 0 && n >= 3) {
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        const char *p = args;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        uint8_t buf[64];
        uint32_t cnt = 0;
        while (*p && cnt < sizeof(buf)) {
            unsigned int v = 0;
            if (sscanf(p, "%2x", &v) != 1) {
                break;
            }
            buf[cnt++] = (uint8_t)v;
            p += 2;
            while (*p == ' ') p++;
        }
        if (cnt == 0) {
            LOG_Printf("no hex data\r\n");
            return;
        }
        /* NOR 写前必须擦：shell 交互自动先擦目标扇区（同扇区数据会被清） */
        uint32_t t0 = HAL_GetTick();
        int erc = ExtStore_EraseRange((ext_part_id_t)id, (uint32_t)a, cnt);
        if (erc != EXT_STORE_OK) {
            LOG_Printf("pre-erase FAIL rc=%d\r\n", erc);
            return;
        }
        int rc = ExtStore_Write((ext_part_id_t)id, (uint32_t)a, buf, cnt);
        LOG_Printf("write %s @0x%lX %luB rc=%d (erase %lu ms)\r\n", pname,
                   (unsigned long)a, (unsigned long)cnt, rc,
                   (unsigned long)(HAL_GetTick() - t0));
        return;
    }

    if ((strcmp(sub, "ws") == 0 || strcmp(sub, "rs") == 0) && n >= 4) {
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        uint32_t slot = (uint32_t)a;
        uint32_t stride = (uint32_t)b;
        if (strcmp(sub, "ws") == 0) {
            const char *p = args;
            int tok = 0;
            while (*p) {
                if (*p == ' ') tok++;
                p++;
                if (tok >= 3) {
                    break;
                }
            }
            while (*p == ' ') p++;
            uint8_t buf[128];
            uint32_t cnt = 0;
            while (*p && cnt < sizeof(buf)) {
                unsigned int v = 0;
                if (sscanf(p, "%2x", &v) != 1) {
                    break;
                }
                buf[cnt++] = (uint8_t)v;
                p += 2;
                while (*p == ' ') p++;
            }
            if (cnt == 0) {
                LOG_Printf("no hex data\r\n");
                return;
            }
            int rc = ExtStore_WriteSafe((ext_part_id_t)id, slot, stride,
                                        buf, cnt);
            LOG_Printf("ws %s slot=%lu stride=%lu %luB rc=%d\r\n",
                       pname, (unsigned long)slot, (unsigned long)stride,
                       (unsigned long)cnt, rc);
        } else {
            uint8_t buf[128];
            uint32_t ver = 0;
            int rc = ExtStore_ReadSafe((ext_part_id_t)id, slot, stride,
                                       buf, sizeof(buf), &ver);
            LOG_Printf("rs rc=%d ver=%lu: ", rc, (unsigned long)ver);
            for (uint32_t i = 0; i < (rc == EXT_STORE_OK ? 16u : 0u); i++) {
                LOG_Printf("%02X ", (unsigned)buf[i]);
            }
            LOG_Printf("\r\n");
        }
        return;
    }

    if (strcmp(sub, "bench") == 0) {
        static uint8_t rbuf[512];   /* 静态：避免命令层栈压力 */
        static uint8_t pat[128];
        static uint8_t chk[128];
        static uint8_t sf[32];
        static uint8_t sf2[32];
        const ext_part_desc_t *user = ExtStore_GetPart(EXT_PART_USER);
        if (user == NULL) {
            return;
        }
        /* 测试区：USER 区最后 1MB（0xE00000-0xEFFFFF） */
        const uint32_t base = user->base + user->size - 1024u * 1024u;
        for (uint32_t i = 0; i < sizeof(pat); i++) {
            pat[i] = (uint8_t)i;
        }

        uint32_t t0 = HAL_GetTick();
        int rc = EXT_STORE_OK;
        for (uint32_t off = 0; off < (1024u * 1024u) && rc == EXT_STORE_OK;
             off += sizeof(rbuf)) {
            rc = ExtStore_Read(EXT_PART_USER, base - user->base + off,
                               rbuf, sizeof(rbuf));
        }
        uint32_t rd_ms = HAL_GetTick() - t0;
        LOG_Printf("store bench read 1MB: rc=%d %lu ms -> %lu KB/s\r\n",
                   rc, (unsigned long)rd_ms,
                   (unsigned long)(rd_ms ? (1024u * 1000u / rd_ms) : 0u));

        t0 = HAL_GetTick();
        rc = ExtStore_EraseRange(EXT_PART_USER, base - user->base, 64u * 1024u);
        LOG_Printf("store bench erase 64KB: rc=%d in %lu ms\r\n",
                   rc, (unsigned long)(HAL_GetTick() - t0));

        t0 = HAL_GetTick();
        rc = EXT_STORE_OK;
        for (uint32_t off = 0; off < (64u * 1024u) && rc == EXT_STORE_OK;
             off += sizeof(pat)) {
            rc = ExtStore_Write(EXT_PART_USER, base - user->base + off,
                                pat, sizeof(pat));
        }
        uint32_t wr_ms = HAL_GetTick() - t0;
        LOG_Printf("store bench write 64KB: rc=%d %lu ms -> %lu KB/s\r\n",
                   rc, (unsigned long)wr_ms,
                   (unsigned long)(wr_ms ? (64u * 1024u * 1000u / wr_ms) : 0u));

        t0 = HAL_GetTick();
        int bad = 0;
        for (uint32_t off = 0; off < (64u * 1024u) && bad == 0;
             off += sizeof(chk)) {
            rc = ExtStore_Read(EXT_PART_USER, base - user->base + off,
                               chk, sizeof(chk));
            if (rc != EXT_STORE_OK || memcmp(chk, pat, sizeof(chk)) != 0) {
                bad = 1;
            }
        }
        LOG_Printf("store bench verify 64KB: rc=%d bad=%d in %lu ms\r\n",
                   rc, bad, (unsigned long)(HAL_GetTick() - t0));

        /* 双份安全写/读校验 */
        for (uint32_t i = 0; i < sizeof(sf); i++) {
            sf[i] = (uint8_t)(0xA0u + i);
        }
        uint32_t sbase = base - user->base + 128u * 1024u;  /* 测试区中部 */
        uint32_t sslot = sbase / (2u * 4096u);  /* 逻辑槽号 = 物理偏移/2stride */
        rc = ExtStore_WriteSafe(EXT_PART_USER, sslot, 4096u, sf, sizeof(sf));
        LOG_Printf("store bench safe-write: rc=%d\r\n", rc);
        uint32_t ver = 0;
        rc = ExtStore_ReadSafe(EXT_PART_USER, sslot, 4096u,
                               sf2, sizeof(sf2), &ver);
        int sbad = (rc != EXT_STORE_OK ||
                    memcmp(sf2, sf, sizeof(sf)) != 0);
        LOG_Printf("store bench safe-read: rc=%d ver=%lu bad=%d\r\n",
                   rc, (unsigned long)ver, sbad);

        /* 恢复：擦回测试区 */
        rc = ExtStore_EraseRange(EXT_PART_USER, base - user->base,
                                 256u * 1024u);
        LOG_Printf("store bench restore: rc=%d\r\n", rc);
        return;
    }

    if (strcmp(sub, "badclear") == 0) {
        ExtStore_BadMapClear();
        return;
    }

    if (strcmp(sub, "bad") == 0 && n >= 3) {
        /* 手动标记坏区（测试/演示坏区管理；badclear 恢复） */
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        ExtStore_MarkBad((ext_part_id_t)id, (uint32_t)a);
        LOG_Printf("marked bad: %s @0x%lX -> isbad=%d\r\n", pname,
                   (unsigned long)a,
                   ExtStore_IsBad((ext_part_id_t)id, (uint32_t)a) ? 1 : 0);
        return;
    }

    if (strcmp(sub, "isbad") == 0 && n >= 3) {
        int id = store_part_parse(pname);
        if (id < 0) {
            LOG_Printf("bad part\r\n");
            return;
        }
        LOG_Printf("isbad %s @0x%lX = %d\r\n", pname, (unsigned long)a,
                   ExtStore_IsBad((ext_part_id_t)id, (uint32_t)a) ? 1 : 0);
        return;
    }

    if (strcmp(sub, "badtest") == 0) {
        /* 坏区表写入路径诊断：标记 + 读回坏区表头 */
        ExtStore_MarkBad(EXT_PART_USER, 0x220000u);
        uint8_t hdr[16] = {0u};
        int rc = ExtStore_Read(EXT_PART_META, 0x2000u, hdr, sizeof(hdr));
        LOG_Printf("badmapA rc=%d: ", rc);
        for (uint32_t i = 0; i < 16u; i++) {
            LOG_Printf("%02X ", (unsigned)hdr[i]);
        }
        LOG_Printf("\r\nisbad=%d\r\n",
                   ExtStore_IsBad(EXT_PART_USER, 0x220000u) ? 1 : 0);
        return;
    }

    LOG_Printf("unknown store sub: %s\r\n", sub);
}

void CmdCatalog_Register(void)
{
    Cmd_Register(cmd_table, CMD_COUNT);
}
