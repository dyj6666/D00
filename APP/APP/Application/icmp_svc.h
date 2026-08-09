/* ================================================================
 * ICMP 服务：板载 Echo 应答 + 完整可观测性（raw API，无独立任务）
 *   - 接管 ICMP Echo Request（type 8），自组 Echo Reply（type 0）
 *   - 统计：收/发/丢、最近 1s 速率与峰值、应答处理耗时（us）、
 *     最近对端与 seq、最小/平均/最大延迟
 *   - 限速（默认 500 pps）与静默模式（reply off = 吞包不回）
 *   - 与 eth_app 的 ping 客户端（EthApp_Ping）天然共存：
 *     echo request 由本服务应答，echo reply 仍由 ping 客户端匹配
 *   - 回调运行在 tcpip 线程，仅做轻量统计与回包，不阻塞
 * ================================================================ */
#ifndef ICMP_SVC_H
#define ICMP_SVC_H

#include <stdint.h>

typedef struct {
    volatile uint32_t echo_rx;      /* 收到的 echo request */
    volatile uint32_t echo_tx;      /* 发出的 echo reply */
    volatile uint32_t echo_drop;    /* 限速/静默丢弃（不回包） */
    volatile uint32_t other_rx;     /* 其他 ICMP 报文（reply/差错等） */
    volatile uint32_t total_rx;
    volatile uint32_t rate_pps;     /* 最近 1s 速率 */
    volatile uint32_t peak_pps;
    volatile uint32_t last_rtt_us;  /* 最近一次应答处理耗时 */
    uint32_t min_rtt_us;
    uint32_t max_rtt_us;
    uint32_t avg_rtt_us;
    uint64_t rtt_sum_us;
    uint32_t rtt_count;
    uint8_t  last_peer[4];
    uint16_t last_seq;
    uint32_t uptime_s;
    uint8_t  enabled;               /* 1=应答 0=静默 */
    uint16_t rate_limit_pps;        /* 限速阈值 */
} icmp_svc_stat_t;

/* 模块初始化（module.c 注册，优先级 66，EthApp 之后） */
void IcmpSvc_Init(void);

const icmp_svc_stat_t *IcmpSvc_GetStat(void);

/* 清零统计（保留 enabled/limit 配置）；`icmp reset` */
void IcmpSvc_Reset(void);

/* 应答开关：1=回包 0=静默吞包；`icmp reply <on|off>` */
int IcmpSvc_SetEnabled(uint8_t on);

/* 限速阈值（pps，1..65535）；超限吞包不回；`icmp limit <pps>` */
int IcmpSvc_SetRateLimit(uint16_t pps);

#endif
