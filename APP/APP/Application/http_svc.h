/* ================================================================
 * HTTP 状态服务（最小 HTTP/1.0，:8080）
 *   - GET /            → HTML 状态页
 *   - GET /api/status  → JSON 状态
 *   - 单连接串行处理，超时关闭；服务任务常驻
 * ================================================================ */
#ifndef HTTP_SVC_H
#define HTTP_SVC_H

#include <stdint.h>

void HttpSvc_Init(void);
void HttpSvc_SetEnabled(uint8_t on);
uint8_t HttpSvc_Enabled(void);
uint32_t HttpSvc_GetRequests(void);

#endif
