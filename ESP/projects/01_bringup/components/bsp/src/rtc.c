/* ================================================================
 * rtc.c —— 片内 RTC / 系统时间
 *
 * 说明：ESP-IDF 已经把 RTC 计数器接到系统时间上（newlib 的 time()），
 *       所以这里只需管理时区 + 提供结构体与时间戳互转，
 *       不重复造轮子去直接操作 RTC 寄存器。
 * ================================================================ */
#include "bsp/rtc.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "BSP/RTC";

static bool s_ready = false;

esp_err_t bsp_rtc_init(const char *tz)
{
    if (s_ready) {
        return ESP_OK;
    }

    const char *zone = (tz != NULL) ? tz : BSP_RTC_TZ_DEFAULT;

    /* setenv 返回非 0 只表示内存不足，这里不致命，故只记日志 */
    if (setenv("TZ", zone, 1) != 0) {
        ESP_LOGW(TAG, "setenv(TZ) failed");
    }
    tzset();

    s_ready = true;
    ESP_LOGI(TAG, "ready (TZ=%s, 片内 RTC 维持，断电丢失)", zone);
    return ESP_OK;
}

esp_err_t bsp_rtc_set(const bsp_datetime_t *dt)
{
    if (dt == NULL || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm t = {0};

    t.tm_year = (int)dt->year - 1900;
    t.tm_mon = (int)dt->month - 1;
    t.tm_mday = (int)dt->day;
    t.tm_hour = (int)dt->hour;
    t.tm_min = (int)dt->minute;
    t.tm_sec = (int)dt->second;
    t.tm_isdst = -1;                    /* 让 libc 自行判断夏令时 */

    const time_t seconds = mktime(&t);

    if (seconds < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    return (settimeofday(&tv, NULL) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_rtc_get(bsp_datetime_t *dt)
{
    if (dt == NULL || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    const time_t now = time(NULL);
    struct tm local = {0};

    if (localtime_r(&now, &local) == NULL) {
        return ESP_FAIL;
    }

    dt->year = (uint16_t)(local.tm_year + 1900);
    dt->month = (uint8_t)(local.tm_mon + 1);
    dt->day = (uint8_t)local.tm_mday;
    dt->hour = (uint8_t)local.tm_hour;
    dt->minute = (uint8_t)local.tm_min;
    dt->second = (uint8_t)local.tm_sec;
    dt->weekday = (uint8_t)local.tm_wday;
    return ESP_OK;
}

esp_err_t bsp_rtc_set_unix(uint32_t seconds)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    const struct timeval tv = { .tv_sec = (time_t)seconds, .tv_usec = 0 };

    return (settimeofday(&tv, NULL) == 0) ? ESP_OK : ESP_FAIL;
}

uint32_t bsp_rtc_get_unix(void)
{
    return (uint32_t)time(NULL);
}

esp_err_t bsp_rtc_format(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 20U) {
        return ESP_ERR_INVALID_ARG;
    }

    const time_t now = time(NULL);
    struct tm local = {0};

    if (localtime_r(&now, &local) == NULL) {
        return ESP_FAIL;
    }

    const size_t n = strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &local);

    return (n > 0U) ? ESP_OK : ESP_FAIL;
}
