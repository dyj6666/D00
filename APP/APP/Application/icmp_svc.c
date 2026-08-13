/* ================================================================
 * icmp_svc —— ICMP 服务：raw PCB 接管 echo，自组回复 + 限速
 *
 * 架构位置：APP 应用层；raw PCB 直连协议栈，独立任务统计
 * 核心流程：收 echo request -> 拷贝载荷改 type=0 -> 软件校验和回复
 * 关键约束：1s 滑动限速窗口；RTT 用 DWT CYCCNT 精确计时(us)
 * ================================================================ */
#include "icmp_svc.h"

#include <string.h>

#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "logger.h"
#include "bsp_system.h"
#include "sysmon.h"

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
    uint32_t now = BSP_GetTick();
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

    uint32_t t0 = BSP_DWT_GetCycleCount();

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
    uint32_t us = (uint32_t)((BSP_DWT_GetCycleCount() - t0) / 168u);
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
    s_win_start_ms = BSP_GetTick();
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
    s_win_start_ms = BSP_GetTick();
    return 0;
}

const icmp_svc_stat_t *IcmpSvc_GetStat(void)
{
    s_stat.uptime_s = (BSP_GetTick() - s_start_ms) / 1000u;
    return &s_stat;
}

/* 监控项：ICMP 统计（由 sysmon 注册表调用） */
void IcmpSvc_PrintStats(void)
{
    const icmp_svc_stat_t *st = IcmpSvc_GetStat();
    LOG_Printf("=== ICMP ===\r\n");
    LOG_Printf("  Echo rx/tx/drop: %lu/%lu/%lu  Other rx: %lu\r\n",
               (unsigned long)st->echo_rx,
               (unsigned long)st->echo_tx,
               (unsigned long)st->echo_drop,
               (unsigned long)st->other_rx);
    LOG_Printf("  Rate: %lu pps (peak %lu)  RTT: %lu/%lu/%lu us\r\n",
               (unsigned long)st->rate_pps,
               (unsigned long)st->peak_pps,
               (unsigned long)st->min_rtt_us,
               (unsigned long)st->avg_rtt_us,
               (unsigned long)st->max_rtt_us);
}

void IcmpSvc_Init(void)
{
    memset((void *)&s_stat, 0, sizeof(s_stat));
    s_stat.enabled = 1;
    s_stat.rate_limit_pps = ICMP_SVC_RATE_LIMIT_DFLT;
    s_start_ms = BSP_GetTick();
    s_win_start_ms = s_start_ms;

    BSP_DWT_Enable();

    s_pcb = raw_new(IP_PROTO_ICMP);
    if (s_pcb == NULL) {
        LOG_Printf("ICMP : WARN raw pcb alloc failed\r\n");
        return;
    }
    raw_bind(s_pcb, IP_ADDR_ANY);
    raw_recv(s_pcb, icmp_svc_recv, NULL);
    SysMon_RegisterItem("ICMP", IcmpSvc_PrintStats);
    LOG_Printf("ICMP : service ready (reply=%u, limit=%u pps)\r\n",
               (unsigned)s_stat.enabled, (unsigned)s_stat.rate_limit_pps);
}
