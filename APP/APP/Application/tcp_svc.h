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

#endif
