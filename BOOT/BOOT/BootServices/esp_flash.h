/* ================================================================
 * esp_flash —— BOOT 侧板载外部 Flash（W25Q128）访问层
 *
 * 架构位置：BOOT BootServices 层；为升级流程提供"读固件包 / 清槽"
 *           能力，与 APP 侧 bsp_w25q128 / ext_store 完全解耦（BOOT
 *           无 RTOS / 无 logger，独立精简实现）。
 *
 * 硬件：SPI1 复用 PB3(SCK)/PB4(MISO)/PB5(MOSI) + PB14(CS)，42MHz。
 * 接口：探测 / 读 / 写 / 4KB 扇区擦（写擦仅用于升级后清槽）。
 * ================================================================ */
#ifndef ESP_FLASH_H
#define ESP_FLASH_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 外部分区（与 APP ext_store 分区表一致） ---------------- */
/* ota_dl 下载区：2MB 分区，当前单槽 1MB（固件包 ≤ 头+832KB+签名） */
#define ESP_OTA_BASE        0x000000u
#define ESP_OTA_DL_SIZE     (1024u * 1024u)

/* img_lib 镜像库：2MB 分区，前 832KB 作为 RUN 备份/回滚源（方案B） */
#define ESP_BACKUP_BASE     0x200000u
#define ESP_BACKUP_SIZE     (917504u)     /* = 14×64KB = 896KB：头(4KB) + APP_SIZE 832KB + 余量 */
#define ESP_BACKUP_HDR_OFF  0x1000u       /* 备份头独立 4KB 扇区，数据紧随其后 */

/* W25Q128 擦除粒度 */
#define ESP_SECTOR_SIZE     4096u
#define ESP_BLOCK64_SIZE    (64u * 1024u)

/* 初始化 SPI1 + GPIO + 探测；true=外部 Flash 在线 */
bool EspFlash_Init(void);

/* 读任意长度（0x03，42MHz 阻塞） */
bool EspFlash_Read(uint32_t off, void *buf, uint32_t len);

/* 写任意长度（0x02 页编程，自动跨页；调用方须先擦） */
bool EspFlash_Write(uint32_t off, const void *data, uint32_t len);

/* 擦 4KB 扇区（off 须 4KB 对齐） */
bool EspFlash_EraseSector(uint32_t off);

/* 擦 64KB 块（off 须 64KB 对齐）——大区域备份/清槽专用，速度快于逐扇区 */
bool EspFlash_EraseBlock64(uint32_t off);

/* 按 64KB 块擦除 [off, off+len)（off/len 均须 64KB 对齐），每块喂狗 */
bool EspFlash_EraseRange64(uint32_t off, uint32_t len);

/* 探测外部下载槽是否含有效固件包（magic 0x4F5441FE，总长 ≤ ESP_OTA_DL_SIZE） */
bool EspFlash_HasPackage(uint32_t *out_total_size);

#endif /* ESP_FLASH_H */
