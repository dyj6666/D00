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

/* 修改静态 IP 并持久化到 flash（上电自动恢复）；`net ip <a.b.c.d>` */
int EthApp_SetStaticIPPersist(const char *addr_str);

/* 恢复出厂默认 IP 192.168.1.10 并清除保存配置；`net ip default` */
int EthApp_SetStaticIPDefault(void);

/* DHCP 客户端：开启后自动获取，超时（15s）回退保存的静态 IP */
#define ETH_DHCP_FALLBACK_MS   15000u
int      EthApp_DhcpStart(void);
int      EthApp_DhcpStop(void);
uint8_t  EthApp_DhcpActive(void);
const char *EthApp_DhcpState(void);

/* 发送一帧 UDP（原始套接字，checksum=0，IPv4 合法）。返回 0 成功 / 负错误码 */
int EthApp_UdpSend(const char *host, uint16_t port,
                   const uint8_t *data, uint16_t len);

/* TX 帧调试：开启后每帧打印前 64 字节（诊断用，默认关闭） */
int  EthApp_SetTxDbg(uint8_t on);
void EthApp_TxDbg(uint32_t len, const uint8_t *buf);
int  EthApp_SetRxDbg(uint8_t on);
void EthApp_RxDbg(uint32_t len, const uint8_t *buf);

/* ---------------- 上位机实时抓帧通道（EthLab 专用） ----------------
 * 开启后，每个 TX/RX 以太网帧经 UDP 发送到抓帧对端 :7778，载荷格式：
 *   dir(1) flags(1) orig_len(2, BE) raw[]   (flags bit0=截断)
 * `net cap on` 由 TCP 控制台发出时自动把对端 IP 设为抓帧目标。 */
int  EthApp_SetCapture(uint8_t on);
int  EthApp_SetCapturePeer(const void *peer4);   /* const ip4_addr_t* */
uint8_t EthApp_GetCaptureOn(void);
uint32_t EthApp_GetCapSent(void);
uint32_t EthApp_GetCapDrop(void);
void EthApp_CapFrame(uint8_t dir, const uint8_t *buf, uint32_t len);
void EthApp_CapFrameP(uint8_t dir, const void *pbuf);   /* const struct pbuf* */

#endif
