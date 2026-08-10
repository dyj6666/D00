/* ================================================================
 * 以太网 TCP OTA 传输服务（服务器角色，端口 9020）
 *   - 协议：0x5A | cmd | len(2 BE) | payload | crc8，逐命令应答；
 *   - 命令：BEGIN/DATA/END/STATUS/RESET，直接驱动 Ota_Begin/Data/End
 *     （与 HOSTLINK 串口 OTA 共用下载核心，互斥使用）；
 *   - 安全：传输内容为加密签名包，校验由 BOOT 统一完成；
 *   - 常驻任务（module.c 注册），单连接串行处理。
 * ================================================================ */
#ifndef OTA_TCP_SVC_H
#define OTA_TCP_SVC_H

#include <stdint.h>

#define OTA_TCP_PORT      9020u

void OtaTcpSvc_Init(void);
uint32_t OtaTcpSvc_GetSessions(void);

#endif
