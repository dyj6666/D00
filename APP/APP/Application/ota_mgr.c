/* ================================================================
 * ota_mgr —— 多协议 OTA 传输注册表实现
 *
 * 架构位置：APP 应用层；被模块初始化 OtaMgr_Init 调用，命令层查询
 * 核心流程：OtaMgr_Init 登记内置传输 -> ota status 逐条展示；
 * CAN 传输由 ota_can_svc 在运行时注册（不再设预留位）
 * ================================================================ */
#include "ota_transport.h"
#include "logger.h"

#include <string.h>

#define OTA_TRANSPORT_MAX  6   /* 注册表容量：UART/TCP/HTTP/CAN + 2 扩展位 */

static ota_transport_t s_table[OTA_TRANSPORT_MAX];  /* 静态表，无堆依赖 */
static uint8_t s_count = 0;                         /* 已注册数量 */

/**
 * @brief  注册一个传输描述到表尾
 * @param  t  传输描述指针（内容按值拷贝）
 * @return 0=成功；-1=空指针或表满；-2=ID 重复
 */
int OtaMgr_Register(const ota_transport_t *t)
{
    if (t == NULL || s_count >= OTA_TRANSPORT_MAX) {
        return -1;
    }
    /* 查重：同一 ID 只允许注册一次 */
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_table[i].id == t->id) {
            return -2;
        }
    }
    s_table[s_count++] = *t;   /* 按值拷贝，调用方可释放自己的副本 */
    LOG_Printf("[OTA] transport %s registered (%s)\r\n",
               t->name, t->available ? "ready" : "reserved");
    return 0;
}

/** @brief 返回当前已注册传输数量（供命令层遍历） */
uint8_t OtaMgr_Count(void)
{
    return s_count;
}

/** @brief 按索引取传输描述；越界返回 NULL */
const ota_transport_t *OtaMgr_Get(uint8_t i)
{
    if (i >= s_count) {
        return NULL;
    }
    return &s_table[i];
}

/** @brief 登记内置三种传输（CAN 由 ota_can_svc 运行时注册） */
void OtaMgr_Init(void)
{
    s_count = 0;                                   /* 幂等：可重复初始化 */
    memset(s_table, 0, sizeof(s_table));           /* 清空旧表，防脏数据 */

    static const ota_transport_t uart = {
        OTA_TRANSPORT_UART, "UART", "HOSTLINK serial (COM13)", 1,
    };
    static const ota_transport_t tcp = {
        OTA_TRANSPORT_ETH_TCP, "ETH-TCP", "TCP server :9020", 1,
    };
    static const ota_transport_t http = {
        OTA_TRANSPORT_ETH_HTTP, "ETH-HTTP", "HTTP client pull", 1,
    };
    (void)OtaMgr_Register(&uart);
    (void)OtaMgr_Register(&tcp);
    (void)OtaMgr_Register(&http);
}
