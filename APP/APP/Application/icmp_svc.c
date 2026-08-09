/* ================================================================
 * ICMP 服务实现：raw PCB 接管 echo request，自组 echo reply
 *   - 自组回包：拷贝 ICMP 载荷（header+data），type=0，软件校验和
 *     （lwipopts.h CHECKSUM_GEN_ICMP=1 仅覆盖协议栈路径，本服务直连 raw）
 *   - 限速窗口：1s 滑动计数，超限吞包不计入丢包以外统计
 *   - RTT 口径：板内应答处理耗时（DWT CYCCNT @168MHz，us）
 * ================================================================ */
#include "icmp_svc.h"

#include <string.h>

#include "main.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "logger.h"

#define ICMP_SVC_RATE_LIMIT_DFLT  500u
#define ICMP_SVC_WINDOW_MS        1000u

static struct raw_pcb *s_pcb = NULL;
static icmp_svc_stat_t s_stat;
static uint32_t s_win_start_ms = 0;
static uint32_t s_win_count = 0;
static uint32_t s_start_ms = 0;

/* 1s 速率窗口（tcpip 线程内调用） */
static void icmp_rate_tick(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - s_win_start_ms) >= ICMP_SVC_WINDOW_MS) {
        s_win_start_ms = now;
        s_win_count = 0;
    }
    s_win_count++;
    s_stat.rate_pps = s_win_count;
    if (s_win_count > s_stat.peak_pps) {
        s_stat.peak_pps = s_win_count;
    }
}

static uint8_t icmp_svc_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)arg;
    (void)pcb;
    if (p == NULL) {
        return 0;
    }
    s_stat.total_rx++;

    /* raw 回调时 payload 仍在 IP 头处：只读偏移、不改动 pbuf */
    struct ip_hdr *iph = (struct ip_hdr *)p->payload;
    u16_t iphlen = IPH_HL_BYTES(iph);
    if (p->tot_len < iphlen + sizeof(struct icmp_echo_hdr)) {
        return 0;   /* 不完整：放行给协议栈 */
    }

    struct icmp_echo_hdr eh;
    if (pbuf_copy_partial(p, &eh, sizeof(eh), iphlen) != sizeof(eh)) {
        return 0;
    }

    if (ICMPH_TYPE(&eh) != ICMP_ECHO) {
        s_stat.other_rx++;
        return 0;   /* 非 echo：放行给 lwIP（差错/应答走默认处理） */
    }

    s_stat.echo_rx++;
    icmp_rate_tick();

    const ip4_addr_t *src4 = ip_2_ip4(addr);
    s_stat.last_peer[0] = ip4_addr1(src4);
    s_stat.last_peer[1] = ip4_addr2(src4);
    s_stat.last_peer[2] = ip4_addr3(src4);
    s_stat.last_peer[3] = ip4_addr4(src4);
    s_stat.last_seq = (uint16_t)(((uint8_t *)&eh.seqno)[0] << 8 |
                                 ((uint8_t *)&eh.seqno)[1]);

    uint32_t t0 = DWT->CYCCNT;

    /* 静默模式或超限：吞包但不回 */
    if (!s_stat.enabled || s_win_count > s_stat.rate_limit_pps) {
        s_stat.echo_drop++;
        s_stat.last_rtt_us = 0;
        pbuf_free(p);
        return 1;
    }

    /* 自组 echo reply：拷贝 ICMP 部分（header+data），type=0，重算校验和 */
    u16_t icmp_len = (u16_t)(p->tot_len - iphlen);
    struct pbuf *r = pbuf_alloc(PBUF_IP, icmp_len, PBUF_RAM);
    if (r == NULL) {
        s_stat.echo_drop++;
        s_stat.last_rtt_us = 0;
        pbuf_free(p);
        return 1;
    }
    pbuf_copy_partial(p, r->payload, icmp_len, iphlen);
    struct icmp_echo_hdr *reply = (struct icmp_echo_hdr *)r->payload;
    ICMPH_TYPE_SET(reply, ICMP_ER);
    ICMPH_CODE_SET(reply, 0);
    reply->chksum = 0;
    reply->chksum = inet_chksum(r->payload, icmp_len);

    if (raw_sendto(s_pcb, r, addr) == ERR_OK) {
        s_stat.echo_tx++;
        uint32_t us = (uint32_t)((DWT->CYCCNT - t0) / 168u);
        s_stat.last_rtt_us = us;
        if (s_stat.rtt_count == 0 || us < s_stat.min_rtt_us) {
            s_stat.min_rtt_us = us;
        }
        if (us > s_stat.max_rtt_us) {
            s_stat.max_rtt_us = us;
        }
        s_stat.rtt_sum_us += us;
        s_stat.rtt_count++;
        s_stat.avg_rtt_us = (uint32_t)(s_stat.rtt_sum_us / s_stat.rtt_count);
    } else {
        s_stat.echo_drop++;
    }
    pbuf_free(r);
    pbuf_free(p);
    return 1;
}

void IcmpSvc_Reset(void)
{
    uint8_t enabled = s_stat.enabled;
    uint16_t limit = s_stat.rate_limit_pps;
    memset((void *)&s_stat, 0, sizeof(s_stat));
    s_stat.enabled = enabled;
    s_stat.rate_limit_pps = limit;
    s_win_count = 0;
    s_win_start_ms = HAL_GetTick();
}

int IcmpSvc_SetEnabled(uint8_t on)
{
    s_stat.enabled = on ? 1u : 0u;
    return 0;
}

int IcmpSvc_SetRateLimit(uint16_t pps)
{
    s_stat.rate_limit_pps = (pps > 0) ? pps : 1u;
    s_win_count = 0;
    s_win_start_ms = HAL_GetTick();
    return 0;
}

const icmp_svc_stat_t *IcmpSvc_GetStat(void)
{
    s_stat.uptime_s = (HAL_GetTick() - s_start_ms) / 1000u;
    return &s_stat;
}

void IcmpSvc_Init(void)
{
    memset((void *)&s_stat, 0, sizeof(s_stat));
    s_stat.enabled = 1;
    s_stat.rate_limit_pps = ICMP_SVC_RATE_LIMIT_DFLT;
    s_start_ms = HAL_GetTick();
    s_win_start_ms = s_start_ms;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_pcb = raw_new(IP_PROTO_ICMP);
    if (s_pcb == NULL) {
        LOG_Printf("ICMP : WARN raw pcb alloc failed\r\n");
        return;
    }
    raw_bind(s_pcb, IP_ADDR_ANY);
    raw_recv(s_pcb, icmp_svc_recv, NULL);
    LOG_Printf("ICMP : service ready (reply=%u, limit=%u pps)\r\n",
               (unsigned)s_stat.enabled, (unsigned)s_stat.rate_limit_pps);
}
