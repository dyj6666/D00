#ifndef __BOOT_CONFIG_H
#define __BOOT_CONFIG_H

#include "main.h"

#define BOOT_BASE_ADDR          0x08000000UL
#define BOOT_SIZE               (64 * 1024)          /* 64 KB, sectors 0-3 */

#define APP_BASE_ADDR           0x08010000UL
#define APP_SIZE                (256 * 1024)         /* 256 KB, sectors 4,5,6 */

#define DOWNLOAD_BASE_ADDR      0x08060000UL         /* NEW: sector 7 start */
#define DOWNLOAD_SIZE           (256 * 1024)         /* 256 KB, sectors 7,8,9,10? actually sector7 is 128KB, so we use 128KB or 256KB? Check: sector7=128KB, sector8=128KB, so 256KB fits perfectly. */

/* Validity marker at end of APP area */
#define APP_VALID_OFFSET        (APP_SIZE - 8)
#define APP_VALID_ADDR          (APP_BASE_ADDR + APP_VALID_OFFSET)
#define APP_VALID_MAGIC         0x4F54412E

#define BOOT_FLAG_NONE          0x0000
#define BOOT_FLAG_UPGRADE       0x5A5A

#define APP_VERSION_ADDR      (APP_VALID_ADDR + 4)   // 版本存在魔数后 4 字节

/* =================== IWDG 相关 =================== */
#define IWDG_PRESCALER          IWDG_PRESCALER_32    // 32 分频, LSI=32.768kHz -> 1.024kHz
#define IWDG_RELOAD             4095                 // 超时 = (4095+1)/1024 ≈ 4s

/* =================== 通信超时 =================== */
#define UART_TIMEOUT            1000                 // ms

#endif /* __BOOT_CONFIG_H */

