/* ================================================================
 * DNS 解析服务（lwIP 客户端封装）
 *   - `dns server <ip>`：配置 DNS 服务器并持久化到 EEPROM（用户数据）
 *   - `dns resolve <host>`：阻塞解析（默认 3s 超时，shell 上下文）
 *   - 无独立任务：解析由调用方阻塞等待，回调在 tcpip 线程执行
 * ================================================================ */
#ifndef DNS_SVC_H
#define DNS_SVC_H

#include <stdint.h>

void DnsSvc_Init(void);                                   /* 加载持久化服务器（幂等） */
int  DnsSvc_SetServer(const char *ip);                    /* 0=成功（写 EEPROM + 生效） */
const uint8_t *DnsSvc_GetServer(void);                    /* NULL=未配置 */
int  DnsSvc_Resolve(const char *host, uint32_t timeout_ms,
                    uint8_t out[4]);                      /* 0=成功 */

#endif
