#ifndef __BOOT_CONFIG_H
#define __BOOT_CONFIG_H

#include "main.h"

/* =====================================================================
 * Flash 分区（STM32F407ZGT6 1MB，扇区对齐核算）
 *   BOOT    0x08000000  64KB   扇区0-3
 *   RUN     0x08010000  320KB  扇区4-6   （APP 链接地址，当前运行区）
 *   BACKUP  0x08060000  384KB  扇区7-9   （上一版固件备份，回滚源，>= RUN）
 *   DOWNLOAD 0x080C0000 128KB  扇区10    （新固件下载暂存区）
 *   PARAM   0x080E0000  128KB  扇区11    （启动标志/回滚计数/升级日志）
 * ===================================================================== */
#define BOOT_BASE_ADDR          0x08000000UL
#define BOOT_SIZE               (64 * 1024)

#define APP_BASE_ADDR           0x08010000UL
#define APP_SIZE                (320 * 1024)      /* RUN 区 */

#define BACKUP_BASE_ADDR        0x08060000UL
#define BACKUP_SIZE             (384 * 1024)

#define DOWNLOAD_BASE_ADDR      0x080C0000UL
#define DOWNLOAD_SIZE           (128 * 1024)

#define PARAM_BASE_ADDR         0x080E0000UL
#define PARAM_SIZE              (128 * 1024)

/* APP 有效性魔数与版本（位于 RUN 区尾部） */
#define APP_VALID_OFFSET        (APP_SIZE - 8)
#define APP_VALID_ADDR          (APP_BASE_ADDR + APP_VALID_OFFSET)
#define APP_VALID_MAGIC         0x4F54412E
#define APP_VERSION_ADDR        (APP_VALID_ADDR + 4)

/* 备份域标志（APP 触发强制升级） */
#define BOOT_FLAG_NONE          0x0000
#define BOOT_FLAG_UPGRADE       0x5A5A

/* =================== 启动状态机 =================== */
#define BOOT_STATE_NORMAL       0x00000001UL   /* 正常运行 */
#define BOOT_STATE_PENDING      0x00000002UL   /* 新固件待确认（启动计数中） */
#define BOOT_STATE_RECOVERY     0x00000003UL   /* 回滚超限，等待上位机 */
#define BOOT_STATE_UPGRADE      0x00000004UL   /* APP 请求升级：进入升级模式 */

#define MAX_BOOT_TRIES          3              /* 新固件最多启动尝试次数 */
#define MAX_ROLLBACK_COUNT      5              /* 连续回滚超过该次数进入 RECOVERY */

/* 参数区结构（双份冗余，防单点损坏） */
#define PARAM_MAGIC             0x50524D54UL   /* "PRMT" */
#define PARAM_SLOT_OFFSET       1024           /* 第二份块偏移 */
#define PARAM_COPY_SIZE         32             /* 结构体对齐后大小 */

#pragma pack(1)
typedef struct {
    uint32_t magic;
    uint32_t boot_state;
    uint32_t boot_count;
    uint32_t rollback_count;
    uint32_t last_error;
    uint32_t last_build_no;   /* 已接受的最大构建号（防重放） */
    uint32_t crc32;
} boot_param_t;
#pragma pack()

/* =================== IWDG =================== */
#define IWDG_PRESCALER          IWDG_PRESCALER_64   /* 500Hz (LSI/64) */
#define IWDG_RELOAD             4095                /* ~8.2s：覆盖 320KB 擦除+复制 */

/* =================== 通信超时 =================== */
#define UART_TIMEOUT            1000

#endif
