/* ================================================================
 * ota_transport —— 多协议 OTA 传输注册表（传输层抽象）
 *
 * 架构位置：APP 应用层；OtaMgr 登记 UART/TCP/HTTP/CAN 传输供命令层查询
 * 核心流程：传输收包 -> 分块喂 Ota_Begin/Data/End -> BOOT 统一安全校验
 * 关键约束：下载核心与传输完全解耦；新增 CAN 仅需实现服务 + Register 一行
 * ================================================================ */
#ifndef OTA_TRANSPORT_H
#define OTA_TRANSPORT_H

#include <stdint.h>

typedef enum {
    OTA_TRANSPORT_UART     = 1,  /* HOSTLINK 串口（现有 data_link 通道） */
    OTA_TRANSPORT_ETH_TCP  = 2,  /* 以太网 TCP 服务器（ota_tcp_svc，:9020） */
    OTA_TRANSPORT_ETH_HTTP = 3,  /* 以太网 HTTP 客户端（ota_http_svc 拉取） */
    OTA_TRANSPORT_CAN      = 4,  /* 预留：CAN 总线 OTA */
} ota_transport_id_t;

typedef struct {
    ota_transport_id_t id;
    const char *name;            /* 显示名，如 "ETH-TCP" */
    const char *desc;            /* 一行说明，如 "TCP server :9020" */
    uint8_t     available;       /* 1=已实现；0=预留（ota status 展示） */
} ota_transport_t;

/** @brief 初始化传输注册表：登记 UART/TCP/HTTP + CAN 预留位 */
void OtaMgr_Init(void);

/**
 * @brief  运行时注册新传输（CAN 等未来接入点）
 * @param  t  传输描述（必须静态存活，注册表只存指针值）
 * @return 0=成功；-1=空指针或表满；-2=ID 重复
 */
int OtaMgr_Register(const ota_transport_t *t);

/** @brief 返回已注册传输数量 */
uint8_t OtaMgr_Count(void);

/** @brief 按索引取传输描述；越界返回 NULL */
const ota_transport_t *OtaMgr_Get(uint8_t i);

#endif /* OTA_TRANSPORT_H */
