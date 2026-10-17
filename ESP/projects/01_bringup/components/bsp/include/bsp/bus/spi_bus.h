/* ================================================================
 * spi_bus.h —— SPI2 共享总线服务层
 *
 * 本板 SPI2 只挂 TF 卡（LCD 走 8080 并口，不占 SPI）。
 * 本层负责：初始化总线（引脚来自 bsp_pinmap.h）+ 提供总线互斥锁。
 *
 * 注意：SDSPI 协议栈会自行调用 spi_bus_add_device()/remove，
 *       所以这里的 bsp_spi_add_device() 只服务于"额外的 SPI 器件"，
 *       TF 卡由 tfcard.c 通过 esp_vfs_fat_sdspi_mount() 挂载。
 * ================================================================ */
#ifndef __BSP_SPI_BUS_H
#define __BSP_SPI_BUS_H

/* 子目录内引用公共头必须带 "bsp/" 前缀，原因见 i2c_bus.h 顶部注释 */
#include "bsp/bsp_types.h"

#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 SPI2 总线（幂等；已初始化时返回 ESP_OK）
 */
esp_err_t bsp_spi_bus_init(void);

/**
 * @brief  释放 SPI2 总线（需先卸载所有器件，含 TF 卡）
 */
esp_err_t bsp_spi_bus_deinit(void);

/**
 * @brief  本层管理的 SPI 主机号（供 SDSPI 等外部协议栈使用）
 */
spi_host_device_t bsp_spi_host(void);

/**
 * @brief  挂载一个 SPI 器件
 */
esp_err_t bsp_spi_add_device(const spi_device_interface_config_t *cfg, spi_device_handle_t *out_dev);

/**
 * @brief  卸载 SPI 器件
 */
esp_err_t bsp_spi_rm_device(spi_device_handle_t dev);

/**
 * @brief  总线加锁 / 解锁（跨器件长事务时使用，例如改 SPI 时钟后连续访问）
 */
esp_err_t bsp_spi_lock(uint32_t timeout_ms);
esp_err_t bsp_spi_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_SPI_BUS_H */
