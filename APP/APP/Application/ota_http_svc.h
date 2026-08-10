/* ================================================================
 * 以太网 HTTP OTA 传输服务（客户端拉取模型）
 *   - `ota http <ip[:port]>/<path>`：GET 下载加密签名固件包
 *   - 解析响应头 Content-Length 得总长，读取包首部 32B 解析版本号，
 *     然后 Ota_Begin → 流式 Ota_Data → Ota_End（共用下载核心）
 *   - 阻塞执行（shell 上下文），进度打日志；安全校验由 BOOT 完成
 * ================================================================ */
#ifndef OTA_HTTP_SVC_H
#define OTA_HTTP_SVC_H

#include <stdint.h>

/* 从 HTTP 服务器拉取固件包；0=成功（含已触发 BOOT 切换） */
int OtaHttp_Download(const char *host, uint16_t port, const char *path);

#endif
