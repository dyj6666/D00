/* ================================================================
 * sntp_svc —— SNTP 时间同步服务：UDP 拉取 + RTC 校准
 *
 * 架构位置：APP 应用层；服务器地址存 EEPROM，独立任务周期同步
 * 核心流程：UDP 请求 -> 解析 NTP 时间戳 -> 写 RTC -> 周期自动同步
 * ================================================================ */
#include "sntp_svc.h"
#include "usr_store.h"
#include "logger.h"
#include "main.h"
#include "rtc.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"

#include <stdio.h>
#include <string.h>

#define SNTP_PORT           123u
#define SNTP_EPOCH_DELTA    2208988800UL   /* 1900→1970 秒差 */
#define SNTP_TZ_OFFSET      28800UL        /* UTC+8（Asia/Shanghai） */
#define SNTP_AUTO_PERIOD_S  3600u
#define SNTP_BOOT_DELAY_S   5u

static uint8_t s_server[4];
static uint8_t s_have_server = 0;
static uint8_t s_auto = 1;

/* 公历秒 → 年月日（Hinnant civil_from_days 算法） */
static void epoch_to_ymd(uint32_t t, uint16_t *y, uint8_t *m, uint8_t *d)
{
    uint32_t days = t / 86400u;
    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t yv = yoe + era * 400u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t dd = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t mm = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    *y = (uint16_t)(yv + ((mm <= 2u) ? 1u : 0u));
    *m = (uint8_t)mm;
    *d = (uint8_t)dd;
}

/* 1970-01-01 为周四：wd = (days + 4) % 7，0=周日；返回 HAL 星期（1=周一..7=周日） */
static uint8_t epoch_weekday(uint32_t t)
{
    uint32_t days = t / 86400u;
    uint32_t wd = (days + 4u) % 7u;
    return (uint8_t)((wd == 0u) ? 7u : wd);
}

int SntpSvc_Sync(const uint8_t server[4], uint16_t port, uint32_t timeout_ms)
{
    if (server == NULL || port == 0u) {
        return -1;
    }
    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        return -2;
    }
    netconn_set_recvtimeout(conn, timeout_ms);
    ip_addr_t dst;
    IP4_ADDR(&dst, server[0], server[1], server[2], server[3]);
    err_t e = netconn_connect(conn, &dst, port);
    if (e != ERR_OK) {
        netconn_delete(conn);
        return -3;
    }

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = 0x1Bu;                       /* LI=0 VN=3 MODE=3（client） */
    struct netbuf *txbuf = netbuf_new();
    if (txbuf == NULL) {
        netconn_delete(conn);
        return -2;
    }
    void *dp = netbuf_alloc(txbuf, sizeof(req));
    if (dp == NULL) {
        netbuf_delete(txbuf);
        netconn_delete(conn);
        return -2;
    }
    memcpy(dp, req, sizeof(req));
    e = netconn_send(conn, txbuf);        /* UDP 用 send，write 仅限 TCP */
    netbuf_delete(txbuf);
    int ret = -4;
    if (e == ERR_OK) {
        struct netbuf *nb = NULL;
        e = netconn_recv(conn, &nb);
        if (e == ERR_OK && nb != NULL) {
            void *data = NULL;
            u16_t len = 0;
            netbuf_data(nb, &data, &len);
            if (len >= 44u) {
                const uint8_t *b = (const uint8_t *)data;
                uint32_t ntp = ((uint32_t)b[40] << 24) |
                               ((uint32_t)b[41] << 16) |
                               ((uint32_t)b[42] << 8) |
                               (uint32_t)b[43];
                if (ntp != 0u) {
                    uint32_t local = ntp - SNTP_EPOCH_DELTA + SNTP_TZ_OFFSET;
                    uint16_t y;
                    uint8_t m, d;
                    epoch_to_ymd(local, &y, &m, &d);
                    uint32_t secs = local % 86400u;

                    RTC_TimeTypeDef rt;
                    rt.Hours = (uint8_t)(secs / 3600u);
                    rt.Minutes = (uint8_t)((secs % 3600u) / 60u);
                    rt.Seconds = (uint8_t)(secs % 60u);
                    rt.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
                    rt.StoreOperation = RTC_STOREOPERATION_RESET;
                    RTC_DateTypeDef rd;
                    rd.Year = (uint8_t)(y % 100u);
                    rd.Month = m;
                    rd.Date = d;
                    rd.WeekDay = epoch_weekday(local);
                    if (HAL_RTC_SetTime(&hrtc, &rt, RTC_FORMAT_BIN) == HAL_OK &&
                        HAL_RTC_SetDate(&hrtc, &rd, RTC_FORMAT_BIN) == HAL_OK) {
                        ret = 0;
                    } else {
                        ret = -5;
                    }
                }
            }
            netbuf_delete(nb);
        }
    }
    netconn_delete(conn);
    return ret;
}

void SntpSvc_GetTimeStr(char *buf, uint32_t len)
{
    if (buf == NULL || len == 0u) {
        return;
    }
    RTC_TimeTypeDef rt;
    RTC_DateTypeDef rd;
    if (HAL_RTC_GetTime(&hrtc, &rt, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&hrtc, &rd, RTC_FORMAT_BIN) != HAL_OK) {
        snprintf(buf, len, "RTC unavailable");
        return;
    }
    snprintf(buf, len, "20%02u-%02u-%02u %02u:%02u:%02u",
             (unsigned)rd.Year, (unsigned)rd.Month, (unsigned)rd.Date,
             (unsigned)rt.Hours, (unsigned)rt.Minutes, (unsigned)rt.Seconds);
}

int SntpSvc_SetServer(const char *ip)
{
    ip4_addr_t a;
    if (ip == NULL || !ip4addr_aton(ip, &a)) {
        return -1;
    }
    s_server[0] = ip4_addr1(&a);
    s_server[1] = ip4_addr2(&a);
    s_server[2] = ip4_addr3(&a);
    s_server[3] = ip4_addr4(&a);
    if (UsrStore_Set(USR_KEY_SNTP_SERVER, s_server, sizeof(s_server)) != 0) {
        return -2;
    }
    s_have_server = 1;
    LOG_Printf("[SNTP] server %u.%u.%u.%u saved to EEPROM\r\n",
               (unsigned)s_server[0], (unsigned)s_server[1],
               (unsigned)s_server[2], (unsigned)s_server[3]);
    return 0;
}

const uint8_t *SntpSvc_GetServer(void)
{
    return s_have_server ? s_server : NULL;
}

int SntpSvc_SetAuto(uint8_t on)
{
    s_auto = on ? 1u : 0u;
    return 0;
}

uint8_t SntpSvc_Auto(void)
{
    return s_auto;
}

/* 自动同步任务：上电 5s 后首同步，之后每小时一次 */
static void sntp_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(SNTP_BOOT_DELAY_S * 1000u));
    for (;;) {
        if (s_auto && s_have_server) {
            int r = SntpSvc_Sync(s_server, SNTP_PORT, 3000u);
            char ts[32];
            SntpSvc_GetTimeStr(ts, sizeof(ts));
            LOG_Printf("[SNTP] sync %s -> %s\r\n",
                       (r == 0) ? "OK" : "FAIL", ts);
        }
        vTaskDelay(pdMS_TO_TICKS(SNTP_AUTO_PERIOD_S * 1000u));
    }
}

void SntpSvc_Init(void)
{
    s_have_server = 0;
    if (UsrStore_Get(USR_KEY_SNTP_SERVER, s_server, sizeof(s_server)) ==
        (int)sizeof(s_server)) {
        s_have_server = 1;
        LOG_Printf("[SNTP] server %u.%u.%u.%u (saved)\r\n",
                   (unsigned)s_server[0], (unsigned)s_server[1],
                   (unsigned)s_server[2], (unsigned)s_server[3]);
    }
    osThreadAttr_t attr = {
        .name = "SntpSvc",
        .stack_size = 1024,
        .priority = osPriorityBelowNormal,
    };
    osThreadNew(sntp_task, NULL, &attr);
}
