/* ================================================================
 * tfcard.h —— 板载 TF 卡（Micro SD，SPI 模式，CS=IO17）
 *
 * 走 SDSPI 协议栈 + FATFS，挂载点固定 /sdcard。
 * 特性：
 *   - 20MHz 时钟（SD 卡在 SPI 模式下的常用上限）、DMA 自动通道；
 *   - 挂载失败不格式化（保护用户数据），可通过 bsp_tfcard_format_if_failed() 放开；
 *   - 提供容量/剩余空间查询，方便做"录满自动停"这类策略。
 * ================================================================ */
#ifndef __BSP_TFCARD_H
#define __BSP_TFCARD_H

#include "bsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_TFCARD_MOUNT_POINT  "/sdcard"   /*!< FATFS 挂载点 */

/**
 * @brief  允许"挂载失败时自动格式化"（默认关闭，须在 init 之前调用）
 */
void bsp_tfcard_format_if_failed(bool enable);

/**
 * @brief  初始化并挂载 TF 卡（幂等；依赖 SPI 总线）
 */
esp_err_t bsp_tfcard_init(void);

/**
 * @brief  卸载并释放（幂等）
 */
esp_err_t bsp_tfcard_deinit(void);

/**
 * @brief  查询是否已挂载
 */
bool bsp_tfcard_is_mounted(void);

/**
 * @brief  重新挂载（先卸载再挂载）
 * @return ESP_OK 成功；其他为失败原因
 *
 * @note   专为**热插拔**准备：TF 卡是随时可插拔的，开机时没卡不代表以后没有。
 *         失败后重试是安全的 —— IDF 的 esp_vfs_fat_sdspi_mount() 在失败路径上会
 *         调用 host 的 deinit（摘掉 SPI 设备）并释放 card，不会泄漏资源。
 */
esp_err_t bsp_tfcard_remount(void);

/**
 * @brief  **格式化后挂载**（会清空卡上全部数据）
 * @return ESP_OK 成功；其他为失败原因
 *
 * @note   专治"卡能应答、但文件系统不是 FAT"——例如出厂即 exFAT 的卡。
 *         走的是 IDF 的 format_if_mount_failed 路径：先正常挂载，挂不上才 f_mkfs
 *         重建 FAT 分区表。如果卡本身没应答（NOT_FOUND/TIMEOUT），这里也会失败，
 *         而且**不会**去动卡上的数据。
 *
 * @warning 调用前请自行确认卡上数据可以丢弃。
 */
esp_err_t bsp_tfcard_format_and_mount(void);

/**
 * @brief  最近一次挂载/卸载的结果
 * @return ESP_OK 表示当前挂载正常；否则是最后一次失败的错误码
 *
 * @note   用于把"为什么没挂上"显示到屏幕上，常见值：
 *         ESP_ERR_NOT_FOUND(0x105) / ESP_ERR_TIMEOUT(0x107) = 卡没应答（没插好/接触不良）
 *         ESP_FAIL(0x101)                                   = 卡应答了但文件系统不是 FAT
 */
esp_err_t bsp_tfcard_last_error(void);

/**
 * @brief  裸 SPI 探测结果（绕过 SDSPI 协议栈的硬件取证）
 */
typedef struct
{
    bool     responded;     /*!< 是否收到卡的 R1 响应（说明 CS/CLK/MOSI/MISO 四线全通） */
    uint8_t  r1_idle;       /*!< CMD0 后的 R1：0x01 = 卡已进入 SPI 空闲态（正常） */
    bool     cmd8_ok;       /*!< CMD8 是否回显 0x1AA（是则卡为 SDHC/SDXC） */
    uint32_t cmd8_tail;     /*!< CMD8 的 4 字节 R7 回显原文 */
} bsp_tfcard_probe_t;

/**
 * @brief  用普通 GPIO 手动敲 CMD0/CMD8 探测卡（**会临时接管 SPI 引脚**）
 * @param  out 探测结果
 * @return ESP_OK 探测完成（结果看 out）；其他为探测本身无法进行（如总线被占用）
 *
 * @note   作用是把"卡的问题"和"驱动配置的问题"一刀切开：
 *         - responded=false        -> 卡在电气层就没应答：卡座接触/卡片/硬件问题
 *         - responded=true 且 R1=01 -> 四线连通、卡工作正常，问题在文件系统或驱动配置
 *         探测完会自动把引脚还给 SPI 总线，不影响后续挂载。
 *         卡已挂载时不要调用（会打断正在进行的文件操作）。
 */
esp_err_t bsp_tfcard_probe_raw(bsp_tfcard_probe_t *out);

/**
 * @brief  获取容量信息
 * @param  total_bytes 总容量（可为 NULL）
 * @param  free_bytes  剩余容量（可为 NULL）
 * @param  fs_name     文件系统类型字符串缓冲（可为 NULL）
 */
esp_err_t bsp_tfcard_info(uint64_t *total_bytes, uint64_t *free_bytes,
                          char *fs_name, size_t fs_name_len);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_TFCARD_H */
