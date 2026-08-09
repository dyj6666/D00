/* ================================================================
 * TCP 服务：工业命令控制台 + 遥测流（netconn API）
 *   - 端口 9000，行协议（CR/LF 结束），最多 2 个并发客户端
 *   - 命令：help/info/ver/sysmon/taskstats/net/led/beep/mpu/echo/stream
 *   - stream on：每秒推送关键遥测（uptime/heap/tasks/ETH）
 * ================================================================ */
#ifndef TCP_SVC_H
#define TCP_SVC_H

#include <stdint.h>

typedef struct {
    volatile uint32_t clients;   /* 当前连接数 */
    uint32_t accepted;           /* 累计接入 */
    uint32_t rejected;           /* 因超限拒绝 */
} tcp_svc_stat_t;

void TcpSvc_Init(void);
const tcp_svc_stat_t *TcpSvc_GetStat(void);

/* TCP 客户端会话（不透明）：统一命令框架 ctx->user 指向它。
 * shell 命令通过 TcpSvc_Client* 接口读取/控制，不直接访问内部。 */
typedef struct tcp_cli tcp_cli_t;

/* 供统一命令表使用：设置/查询当前 TCP 客户端的遥测流开关 */
int     TcpSvc_ClientSetStream(tcp_cli_t *cli, uint8_t on);
uint8_t TcpSvc_ClientStream(tcp_cli_t *cli);

/* 取 TCP 对端 IPv4 地址（抓帧目标自动探测用），成功返回 0 */
int TcpSvc_ClientPeerIP(tcp_cli_t *cli, uint8_t ip[4]);

#endif
