/* ================================================================
 * 以太网应用模块：链路/IP/流量状态 + ICMP ping（shell/LCD/sysmon 共用）
 * ================================================================ */
#ifndef ETH_APP_H
#define ETH_APP_H

#include <stdint.h>

typedef struct {
    uint8_t  link_up;       /* 物理链路（PHY link）状态 */
    uint8_t  ip[4];         /* IPv4 地址 */
    uint8_t  gw[4];         /* 网关 */
    uint8_t  mac[6];        /* MAC 地址 */
    uint32_t rx_packets;    /* 收到的以太网帧数（ETH 中断回调计数） */
    uint32_t tx_packets;    /* 发出的以太网帧数 */
    uint32_t link_uptime_s; /* 链路持续秒数 */
} eth_status_t;

/* 模块初始化（module.c 注册，优先级 65） */
void EthApp_Init(void);

const eth_status_t *EthApp_GetStatus(void);

/* 链路变化回调（lwip.c 的 link callback USER CODE 区调用） */
void EthApp_SetLinkState(uint8_t up);

/* 帧计数钩子（ethernetif.c 的 HAL_ETH_*CpltCallback 调用） */
void EthApp_CountRx(void);
void EthApp_CountTx(void);

/* 1s 刷新：同步 IP/MAC/链路时长（渲染任务/命令上下文调用） */
void EthApp_RefreshStatus(void);

/* ICMP ping：返回 RTT(ms)；<0 为错误码（-1 地址无效/-2 链路未通/
 * -3 内存/-4 发送失败/-5 超时）。阻塞 timeout_ms（shellTask 上下文）。 */
int EthApp_Ping(const char *host, uint32_t timeout_ms);

/* 运行时修改静态 IP（/24，无网关；经 tcpip 回调线程安全切换），
 * 返回 0 成功 / -1 地址无效 / -2 回调投递失败 */
int EthApp_SetStaticIP(const char *addr_str);

/* 发送一帧 UDP（原始套接字，checksum=0，IPv4 合法）。返回 0 成功 / 负错误码 */
int EthApp_UdpSend(const char *host, uint16_t port,
                   const uint8_t *data, uint16_t len);

/* TX 帧调试：开启后每帧打印前 64 字节（诊断用，默认关闭） */
int  EthApp_SetTxDbg(uint8_t on);
void EthApp_TxDbg(uint32_t len, const uint8_t *buf);

#endif
