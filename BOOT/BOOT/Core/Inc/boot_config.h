#ifndef __BOOT_CONFIG_H
#define __BOOT_CONFIG_H

#include "main.h"

/* =================== Flash 分区定义 =================== */
#define BOOT_BASE_ADDR          0x08000000UL
#define BOOT_SIZE               (64 * 1024)          // 64 KB

#define APP_BASE_ADDR           0x08010000UL         // APP 起始
#define APP_SIZE                (256 * 1024)         // 256 KB

#define DOWNLOAD_BASE_ADDR      0x08050000UL         // 下载缓存区起始
#define DOWNLOAD_SIZE           (256 * 1024)         // 与 APP 等大

/* =================== 升级标志 =================== */
#define BOOT_FLAG_NONE          0x0000
#define BOOT_FLAG_UPGRADE       0x5A5A               // 任意非零值

/* =================== IWDG 相关 =================== */
#define IWDG_PRESCALER          IWDG_PRESCALER_32    // 32 分频, LSI=32.768kHz -> 1.024kHz
#define IWDG_RELOAD             4095                 // 超时 = (4095+1)/1024 ≈ 4s

/* =================== 通信超时 =================== */
#define UART_TIMEOUT            1000                 // ms

#endif /* __BOOT_CONFIG_H */

