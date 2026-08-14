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

/* 外部 ota_dl 分区槽 0 的 Flash 偏移（与 APP ext_store 分区表一致） */
#define ESP_OTA_BASE    0x000000u

/* 初始化 SPI1 + GPIO + 探测；true=外部 Flash 在线 */
bool EspFlash_Init(void);

/* 读任意长度（0x03，42MHz 阻塞） */
bool EspFlash_Read(uint32_t off, void *buf, uint32_t len);

/* 写任意长度（0x02 页编程，自动跨页；调用方须先擦） */
bool EspFlash_Write(uint32_t off, const void *data, uint32_t len);

/* 擦 4KB 扇区（off 须 4KB 对齐） */
bool EspFlash_EraseSector(uint32_t off);

/* 探测外部槽 0 是否含有效固件包（magic 0x4F5441FE） */
bool EspFlash_HasPackage(uint32_t *out_total_size);

#endif /* ESP_FLASH_H */
