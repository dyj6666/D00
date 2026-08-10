/* ================================================================
 * 多协议 OTA 传输注册表（传输层抽象）
 *
 * 架构：OTA 下载核心（ota_agent.c：会话槽/DOWNLOAD 区/BOOT 触发）
 *       与传输方式完全解耦。任何传输（UART/TCP/HTTP/CAN...）只需把
 *       固件包分块喂给 Ota_Begin/Ota_Data/Ota_End 即可，安全校验
 *       （AES/ECDSA/防回滚）由 BOOT 统一完成。
 *
 * 新增传输（如未来 CAN）三步接入：
 *   1) 在 ota_transport_id_t 增加枚举；
 *   2) 实现传输服务（收包 → 调 Ota_Begin/Data/End）；
 *   3) 在 OtaMgr_Init 或运行时 OtaMgr_Register 登记，`ota status`
 *      即可展示，命令层零改动。
 * ================================================================ */
#ifndef OTA_TRANSPORT_H
#define OTA_TRANSPORT_H

#include <stdint.h>

typedef enum {
    OTA_TRANSPORT_UART    = 1,   /* HOSTLINK 串口（现有 data_link 通道） */
    OTA_TRANSPORT_ETH_TCP = 2,   /* 以太网 TCP 服务器（ota_tcp_svc，:9020） */
    OTA_TRANSPORT_ETH_HTTP = 3,  /* 以太网 HTTP 客户端（ota_http_svc 拉取） */
    OTA_TRANSPORT_CAN     = 4,   /* 预留：CAN 总线 OTA */
} ota_transport_id_t;

typedef struct {
    ota_transport_id_t id;
    const char *name;            /* 显示名 */
    const char *desc;            /* 说明 */
    uint8_t     available;       /* 1=已实现 0=预留 */
} ota_transport_t;

/* 初始化：登记内置传输（UART/TCP/HTTP + CAN 预留位） */
void OtaMgr_Init(void);

/* 运行时注册新传输（CAN 等未来接入点）；返回 0=成功 */
int OtaMgr_Register(const ota_transport_t *t);

uint8_t OtaMgr_Count(void);
const ota_transport_t *OtaMgr_Get(uint8_t i);

#endif
