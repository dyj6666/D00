/* ================================================================
 * 以太网应用模块实现
 *   - 状态：链路/IP/MAC/收发帧计数（中断回调轻量计数）
 *   - 诊断：ICMP ping（raw API，shellTask 上下文阻塞）
 *   - 无独立任务：1s 数据同步由 LCD/sysmon/shell 展示侧驱动
 * ================================================================ */
#include "eth_app.h"

#include <string.h>

#include "main.h"
#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"
#include "cmsis_os2.h"
#include "logger.h"
#include "var_manager.h"
#include "var_ids.h"

extern struct netif gnetif;   /* lwip.c 全局网络接口 */

static eth_status_t s_eth;
static uint32_t s_link_start_ms = 0;
static volatile uint8_t s_tx_dbg = 0;

/* ---------------- ICMP ping（raw PCB + 信号量） ---------------- */
static struct raw_pcb *s_ping_pcb = NULL;
static struct raw_pcb *s_udp_pcb = NULL;
static struct raw_pcb *s_echo_pcb = NULL;

#define UDP_ECHO_PORT   7777
static osSemaphoreId_t s_ping_sem = NULL;
static volatile uint16_t s_ping_id;
static volatile uint16_t s_ping_seq;
static volatile uint8_t  s_ping_done;
static volatile uint32_t s_ping_rtt_ms;
static volatile uint32_t s_ping_start_ms;

static uint8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    if (p == NULL) {
        return 0;
    }
    /* raw 回调时 payload 仍在 IP 头处：先跳过 IP 头（含选项） */
    if (p->len < IP_HLEN) {
        return 0;
    }
    if (pbuf_remove_header(p, IPH_HL_BYTES((struct ip_hdr *)p->payload)) != 0) {
        return 0;
    }

    struct icmp_echo_hdr reply;
    uint8_t want_id_hi = (uint8_t)((uint16_t)s_ping_id >> 8);
    uint8_t want_id_lo = (uint8_t)((uint16_t)s_ping_id & 0xFF);
    uint8_t want_seq_hi = (uint8_t)((uint16_t)s_ping_seq >> 8);
    uint8_t want_seq_lo = (uint8_t)((uint16_t)s_ping_seq & 0xFF);
    if (p->tot_len >= sizeof(reply) &&
        pbuf_copy_partial(p, &reply, sizeof(reply), 0) == sizeof(reply) &&
        ICMPH_TYPE(&reply) == ICMP_ER &&
        ((uint8_t *)&reply.id)[0] == want_id_hi &&
        ((uint8_t *)&reply.id)[1] == want_id_lo &&
        ((uint8_t *)&reply.seqno)[0] == want_seq_hi &&
        ((uint8_t *)&reply.seqno)[1] == want_seq_lo) {
        s_ping_rtt_ms = (uint32_t)(HAL_GetTick() - s_ping_start_ms);
        s_ping_done = 1;
        pbuf_free(p);
        if (s_ping_sem != NULL) {
            osSemaphoreRelease(s_ping_sem);
        }
        return 1;   /* 匹配本 ping 的应答：消费 */
    }
    return 0;       /* 其余 ICMP（echo 请求等）放行给协议栈处理 */
}

int EthApp_Ping(const char *host, uint32_t timeout_ms)
{
    ip4_addr_t dst;
    if (host == NULL || !ip4addr_aton(host, &dst)) {
        return -1;
    }
    if (!netif_is_link_up(&gnetif)) {
        return -2;
    }
    if (s_ping_sem == NULL) {
        s_ping_sem = osSemaphoreNew(1, 0, NULL);
    }
    if (s_ping_pcb == NULL) {
        s_ping_pcb = raw_new(IP_PROTO_ICMP);
        if (s_ping_pcb == NULL) {
            return -3;
        }
        raw_bind(s_ping_pcb, IP_ADDR_ANY);
        raw_recv(s_ping_pcb, ping_recv, NULL);
    }

    s_ping_id = (uint16_t)(HAL_GetTick() & 0xFFFFu);
    s_ping_seq++;
    s_ping_done = 0;
    s_ping_start_ms = HAL_GetTick();

    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32u,
                                PBUF_RAM);
    if (p == NULL) {
        return -3;
    }
    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->chksum = 0;
    ((uint8_t *)&echo->id)[0]    = (uint8_t)((uint16_t)s_ping_id >> 8);
    ((uint8_t *)&echo->id)[1]    = (uint8_t)((uint16_t)s_ping_id & 0xFF);
    ((uint8_t *)&echo->seqno)[0] = (uint8_t)((uint16_t)s_ping_seq >> 8);
    ((uint8_t *)&echo->seqno)[1] = (uint8_t)((uint16_t)s_ping_seq & 0xFF);
    memset((uint8_t *)p->payload + sizeof(struct icmp_echo_hdr), 0xAA, 32u);
    echo->chksum = inet_chksum(p->payload, p->len);

    ip_addr_t dst_addr;
    ip_addr_copy_from_ip4(dst_addr, dst);
    err_t err = raw_sendto(s_ping_pcb, p, &dst_addr);
    pbuf_free(p);
    if (err != ERR_OK) {
        return -4;
    }

    while (!s_ping_done && (HAL_GetTick() - s_ping_start_ms) < timeout_ms) {
        osSemaphoreAcquire(s_ping_sem, 50u);
    }
    return s_ping_done ? (int)s_ping_rtt_ms : -5;
}

/* ---------------- 运行时改 IP（tcpip 线程安全） ---------------- */

typedef struct {
    ip4_addr_t ip;
    ip4_addr_t mask;
    ip4_addr_t gw;
} net_addr_arg_t;

static net_addr_arg_t s_net_addr;

static void netif_set_addr_cb(void *arg)
{
    const net_addr_arg_t *a = (const net_addr_arg_t *)arg;
    netif_set_addr(&gnetif, &a->ip, &a->mask, &a->gw);
}

int EthApp_SetStaticIP(const char *addr_str)
{
    ip4_addr_t ip;
    if (addr_str == NULL || !ip4addr_aton(addr_str, &ip)) {
        return -1;
    }
    s_net_addr.ip = ip;
    IP4_ADDR(&s_net_addr.mask, 255, 255, 255, 0);
    IP4_ADDR(&s_net_addr.gw, 0, 0, 0, 0);   /* 直连场景无网关 */
    if (tcpip_callback(netif_set_addr_cb, &s_net_addr) != ERR_OK) {
        return -2;
    }
    return 0;
}

/* ---------------- UDP 诊断发送（raw API，checksum=0） ---------------- */

int EthApp_UdpSend(const char *host, uint16_t port,
                   const uint8_t *data, uint16_t len)
{
    ip4_addr_t dst;
    if (host == NULL || !ip4addr_aton(host, &dst)) {
        return -1;
    }
    if (!netif_is_link_up(&gnetif)) {
        return -2;
    }
    if (len > 1400u) {
        return -6;   /* 诊断帧限长，避免分片 */
    }

    if (s_udp_pcb == NULL) {
        s_udp_pcb = raw_new(IP_PROTO_UDP);
        if (s_udp_pcb == NULL) {
            return -3;
        }
        raw_bind(s_udp_pcb, IP_ADDR_ANY);
    }

    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(8u + len), PBUF_RAM);
    if (p == NULL) {
        return -3;
    }
    uint8_t *u = (uint8_t *)p->payload;
    u[0] = (uint8_t)(port >> 8);      /* sport = dport */
    u[1] = (uint8_t)(port & 0xFF);
    u[2] = (uint8_t)(port >> 8);
    u[3] = (uint8_t)(port & 0xFF);
    u[4] = (uint8_t)((8u + len) >> 8);
    u[5] = (uint8_t)((8u + len) & 0xFF);
    u[6] = 0;                          /* checksum = 0（IPv4 合法） */
    u[7] = 0;
    if (len > 0u && data != NULL) {
        memcpy(u + 8, data, len);
    }

    ip_addr_t dst_addr;
    ip_addr_copy_from_ip4(dst_addr, dst);
    err_t err = raw_sendto(s_udp_pcb, p, &dst_addr);
    pbuf_free(p);
    return (err == ERR_OK) ? 0 : -4;
}

/* ---------------- UDP 回显服务（端口 7777，供 RTT/吞吐验证） ---------------- */

static uint8_t udp_echo_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    if (p == NULL || p->tot_len < 8) {
        return 0;
    }
    /* raw 回调时 payload 仍在 IP 头处：先跳过 IP 头（含选项） */
    if (p->len < IP_HLEN) {
        return 0;
    }
    if (pbuf_remove_header(p, IPH_HL_BYTES((struct ip_hdr *)p->payload)) != 0) {
        return 0;
    }

    /* UDP 头：offset2 = 目的端口。用显式字节比较（与字节序无关） */
    uint8_t dport_b[2];
    pbuf_copy_partial(p, dport_b, 2, 2);
    if (dport_b[0] != (uint8_t)(UDP_ECHO_PORT >> 8) ||
        dport_b[1] != (uint8_t)(UDP_ECHO_PORT & 0xFF)) {
        return 0;   /* 非回显端口：放行给协议栈 */
    }

    /* 整包拷贝后交换源/目的端口回显（checksum=0，IPv4 合法） */
    struct pbuf *r = pbuf_alloc(PBUF_IP, p->tot_len, PBUF_RAM);
    if (r != NULL) {
        pbuf_copy_partial(p, r->payload, p->tot_len, 0);
        uint8_t *u = (uint8_t *)r->payload;
        uint8_t sport_hi = u[0], sport_lo = u[1];
        u[0] = dport_b[0];                 /* 回显：原目的端口变源 */
        u[1] = dport_b[1];
        u[2] = sport_hi;                   /* 原源端口变目的 */
        u[3] = sport_lo;
        u[6] = 0;                          /* UDP checksum = 0 */
        u[7] = 0;
        raw_sendto(s_echo_pcb, r, addr);
        pbuf_free(r);
    }
    pbuf_free(p);
    return 1;
}

/* ---------------- 状态 ---------------- */

void EthApp_SetLinkState(uint8_t up)
{
    if (up && !s_eth.link_up) {
        s_link_start_ms = HAL_GetTick();   /* 链路刚建立，起算时长 */
    }
    s_eth.link_up = up ? 1 : 0;
}

void EthApp_CountRx(void)
{
    s_eth.rx_packets++;
}

void EthApp_CountTx(void)
{
    s_eth.tx_packets++;
}

int EthApp_SetTxDbg(uint8_t on)
{
    s_tx_dbg = on ? 1u : 0u;
    return 0;
}

void EthApp_TxDbg(uint32_t len, const uint8_t *buf)
{
    if (!s_tx_dbg || buf == NULL) {
        return;
    }
    uint32_t n = (len < 96u) ? len : 96u;
    LOG_Printf("TX %luB:", (unsigned long)len);
    for (uint32_t i = 0; i < n; i++) {
        LOG_Printf(" %02X", buf[i]);
    }
    LOG_Printf("\r\n");
}

const eth_status_t *EthApp_GetStatus(void)
{
    return &s_eth;
}

void EthApp_RefreshStatus(void)
{
    const struct netif *ni = &gnetif;
    s_eth.link_up = netif_is_link_up(ni) ? 1 : 0;

    if (s_eth.link_up) {
        const ip4_addr_t *a = ip_2_ip4(&ni->ip_addr);
        const ip4_addr_t *g = ip_2_ip4(&ni->gw);
        s_eth.ip[0] = ip4_addr1(a);
        s_eth.ip[1] = ip4_addr2(a);
        s_eth.ip[2] = ip4_addr3(a);
        s_eth.ip[3] = ip4_addr4(a);
        s_eth.gw[0] = ip4_addr1(g);
        s_eth.gw[1] = ip4_addr2(g);
        s_eth.gw[2] = ip4_addr3(g);
        s_eth.gw[3] = ip4_addr4(g);
        s_eth.link_uptime_s = (HAL_GetTick() - s_link_start_ms) / 1000u;
    } else {
        s_eth.link_uptime_s = 0;
    }
    for (int i = 0; i < 6; i++) {
        s_eth.mac[i] = ni->hwaddr[i];
    }
}

void EthApp_Init(void)
{
    memset(&s_eth, 0, sizeof(s_eth));
    VAR_Register(VAR_ID_ETH_LINK, "eth_link", VAR_TYPE_INT32, 0,
                 &s_eth.link_up);
    VAR_Register(VAR_ID_ETH_RX,   "eth_rx",   VAR_TYPE_INT32, 0,
                 &s_eth.rx_packets);
    VAR_Register(VAR_ID_ETH_TX,   "eth_tx",   VAR_TYPE_INT32, 0,
                 &s_eth.tx_packets);
    s_echo_pcb = raw_new(IP_PROTO_UDP);
    if (s_echo_pcb != NULL) {
        raw_bind(s_echo_pcb, IP_ADDR_ANY);
        raw_recv(s_echo_pcb, udp_echo_recv, NULL);
    } else {
        LOG_Printf("ETH  : WARN udp echo pcb alloc failed\r\n");
    }
    LOG_Printf("ETH  : app ready (static IP 192.168.1.10/24)\r\n");
}
