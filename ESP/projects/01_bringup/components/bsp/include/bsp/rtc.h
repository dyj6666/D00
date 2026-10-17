/* ================================================================
 * rtc.h —— 片内 RTC / 系统时间
 *
 * 板上没有独立 RTC 芯片（也无纽扣电池），时间由 ESP32-S3 的 RTC 域维持：
 *   - 上电后需要先 bsp_rtc_set() 校准一次；
 *   - 软重启（不断电）时间会保持；
 *   - 完全断电后时间丢失，重新上电需再校准。
 * 需要长时间准确计时请外挂 RTC 或开机联网对时。
 * ================================================================ */
#ifndef __BSP_RTC_H
#define __BSP_RTC_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认时区：中国标准时间（POSIX 写法 CST-8 = UTC+8） */
#define BSP_RTC_TZ_DEFAULT      "CST-8"

/**
 * @brief  初始化系统时间（幂等）：设置时区并启用
 * @param  tz POSIX 时区字符串，NULL 用 BSP_RTC_TZ_DEFAULT
 */
esp_err_t bsp_rtc_init(const char *tz);

/**
 * @brief  设置时间（本地时间）
 */
esp_err_t bsp_rtc_set(const bsp_datetime_t *dt);

/**
 * @brief  读取时间（本地时间）
 */
esp_err_t bsp_rtc_get(bsp_datetime_t *dt);

/**
 * @brief  设置 Unix 时间戳（秒）
 */
esp_err_t bsp_rtc_set_unix(uint32_t seconds);

/**
 * @brief  读取 Unix 时间戳（秒）
 */
uint32_t bsp_rtc_get_unix(void);

/**
 * @brief  格式化成 "YYYY-MM-DD HH:MM:SS"
 * @param  buf 输出缓冲（建议 >= 24 字节）
 */
esp_err_t bsp_rtc_format(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_RTC_H */
