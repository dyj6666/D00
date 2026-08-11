/* ================================================================
 * dns_svc —— DNS 解析服务：服务器持久化 + 查询封装
 *
 * 架构位置：APP 应用层；服务器地址存 EEPROM(usr_store)，供各服务复用
 * 核心流程：dns_setserver 配置 -> lwIP dns_gethostbyname 异步解析
 * ================================================================ */
#include "dns_svc.h"
#include "usr_store.h"
#include "logger.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "cmsis_os2.h"

#include <string.h>

static uint8_t s_server[4];
static uint8_t s_have_server = 0;

typedef struct {
    osSemaphoreId_t sem;
    volatile uint8_t done;
    volatile uint8_t ok;
    volatile uint8_t ip[4];
} dns_req_t;

static dns_req_t s_req;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    dns_req_t *r = (dns_req_t *)arg;
    r->done = 1;
    if (ipaddr != NULL) {
        r->ok = 1;
        const ip4_addr_t *a4 = ip_2_ip4(ipaddr);
        r->ip[0] = ip4_addr1(a4);
        r->ip[1] = ip4_addr2(a4);
        r->ip[2] = ip4_addr3(a4);
        r->ip[3] = ip4_addr4(a4);
    }
    if (r->sem != NULL) {
        osSemaphoreRelease(r->sem);
    }
}

void DnsSvc_Init(void)
{
    s_have_server = 0;
    if (UsrStore_Get(USR_KEY_DNS_SERVER, s_server, sizeof(s_server)) ==
        (int)sizeof(s_server)) {
        ip_addr_t a;
        IP4_ADDR(&a, s_server[0], s_server[1], s_server[2], s_server[3]);
        dns_setserver(0, &a);
        s_have_server = 1;
        LOG_Printf("[DNS] server %u.%u.%u.%u (saved)\r\n",
                   (unsigned)s_server[0], (unsigned)s_server[1],
                   (unsigned)s_server[2], (unsigned)s_server[3]);
    } else {
        LOG_Printf("[DNS] no saved server, use `dns server <ip>`\r\n");
    }
}

int DnsSvc_SetServer(const char *ip)
{
    ip4_addr_t a;
    if (ip == NULL || !ip4addr_aton(ip, &a)) {
        return -1;
    }
    s_server[0] = ip4_addr1(&a);
    s_server[1] = ip4_addr2(&a);
    s_server[2] = ip4_addr3(&a);
    s_server[3] = ip4_addr4(&a);
    if (UsrStore_Set(USR_KEY_DNS_SERVER, s_server, sizeof(s_server)) != 0) {
        return -2;
    }
    ip_addr_t ia;
    ip_addr_copy_from_ip4(ia, a);
    dns_setserver(0, &ia);
    s_have_server = 1;
    LOG_Printf("[DNS] server %u.%u.%u.%u saved to EEPROM\r\n",
               (unsigned)s_server[0], (unsigned)s_server[1],
               (unsigned)s_server[2], (unsigned)s_server[3]);
    return 0;
}

const uint8_t *DnsSvc_GetServer(void)
{
    return s_have_server ? s_server : NULL;
}

int DnsSvc_Resolve(const char *host, uint32_t timeout_ms, uint8_t out[4])
{
    if (host == NULL || out == NULL) {
        return -1;
    }
    if (s_req.sem == NULL) {
        s_req.sem = osSemaphoreNew(1, 0, NULL);
    }
    osSemaphoreId_t sem = s_req.sem;
    memset(&s_req, 0, sizeof(s_req));
    s_req.sem = sem;

    ip_addr_t addr;
    err_t e = dns_gethostbyname(host, &addr, dns_found_cb, &s_req);
    if (e == ERR_OK) {
        const ip4_addr_t *a4 = ip_2_ip4(&addr);
        out[0] = ip4_addr1(a4);
        out[1] = ip4_addr2(a4);
        out[2] = ip4_addr3(a4);
        out[3] = ip4_addr4(a4);
        return 0;
    }
    if (e != ERR_INPROGRESS) {
        return -2;
    }
    osSemaphoreAcquire(s_req.sem, timeout_ms);
    if (!s_req.done) {
        return -3;                       /* 超时 */
    }
    if (!s_req.ok) {
        return -4;                       /* NXDOMAIN/无应答 */
    }
    out[0] = s_req.ip[0];
    out[1] = s_req.ip[1];
    out[2] = s_req.ip[2];
    out[3] = s_req.ip[3];
    return 0;
}
