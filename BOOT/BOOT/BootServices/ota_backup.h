/* ================================================================
 * ota_backup —— BOOT 侧外部备份/回滚存储服务（方案B）
 *
 * 架构位置：BOOT BootServices 层；位于 esp_flash（SPI 驱动）之上、
 *           boot_app（升级业务）之下。内部 BACKUP 区取消后，升级前
 *           将当前 RUN 全量备份到外部 Flash img_lib 分区，回滚/自动
 *           修复由本层统一完成，业务不感知 SPI 细节。
 *
 * 布局（img_lib 分区，偏移相对 ESP_BACKUP_BASE）：
 *   [0x0000, 0x1000) 备份头（32B 有效，4KB 扇区独立，防数据覆盖向量表）
 *   [0x1000, ...)     RUN 全量镜像（APP_SIZE 832KB）
 *
 * 安全设计：备份完成后整槽读回逐块比对（写后校验），校验不过即视为
 *           备份失败并中止升级；恢复前校验头 CRC + 魔数；升级成功后
 *           清空备份槽防重放。全程每块喂狗，配合 IWDG 16.4s 兜底。
 * ================================================================ */
#ifndef OTA_BACKUP_H
#define OTA_BACKUP_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 备份头（32B，独立 4KB 扇区） ---------------- */
#define OTA_BACKUP_MAGIC        0x31504B42u   /* 'BKP1' */

#pragma pack(1)
typedef struct {
    uint32_t magic;        /* OTA_BACKUP_MAGIC */
    uint32_t app_size;     /* 备份的固件大小（= APP_SIZE，全量快照） */
    uint32_t build_no;     /* 备份时刻 RUN 版本号（来自 APP_VERSION_ADDR） */
    uint32_t crc32;        /* 头字段 CRC32（magic..crc32 之前） */
    uint32_t reserved[4];  /* 对齐 32B，预留扩展 */
} ota_backup_header_t;
#pragma pack()

/* 探测外部 Flash（esp_flash 初始化 + 备份区大小自检） */
bool OtaBackup_Init(void);

/* 备份槽是否有有效备份（头魔数 + CRC 校验通过） */
bool OtaBackup_IsValid(void);

/* 将当前 RUN 全量备份到外部槽：擦除 → 写头 → 写数据 → 读回逐块校验 */
bool OtaBackup_Save(void);

/* 从外部槽恢复 RUN：校验头 → 擦 RUN → 写数据 → 补魔数/版本 */
bool OtaBackup_Restore(void);

/* 清空备份槽（升级成功/防重放） */
void OtaBackup_Clear(void);

#endif /* OTA_BACKUP_H */
