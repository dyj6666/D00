/* ================================================================
 * i2c_bus.h —— I2C0 共享总线服务层
 *
 * 全板 6 个 I2C 器件（XL9555 / AT24C02 / AP3216C / QMI8658A /
 * CHSC5432 / ES8311）共用 I2C0。本层职责：
 *   1. 唯一持有 I2C 主机总线句柄，器件驱动不再各自初始化；
 *   2. 提供"事务级"互斥锁，保证复合时序（写寄存器+读数据）不被插入；
 *   3. 统一超时与错误统计，出错日志限流，避免刷屏拖慢系统。
 *
 * 性能约定：一次"读寄存器"= 一次 write_read（写地址 + 重复起始 + 读），
 *           不做"写寄存器 / 停 / 再起始 / 读"这种两段式，省掉一次总线仲裁。
 * ================================================================ */
#ifndef __BSP_I2C_BUS_H
#define __BSP_I2C_BUS_H

/*
 * 注意：本文件位于 include/bsp/bus/ 子目录，引用同组件的公共头必须带 "bsp/" 前缀。
 * 若只写 "bsp_types.h"，编译器会先按本文件目录找（bsp/bus/bsp_types.h，不存在），
 * 再按包含路径找（包含路径是 components/bsp/include，同样没有），从而报
 * "bsp_types.h: No such file or directory"。
 */
#include "bsp/bsp_types.h"

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 已挂载的 I2C 从设备句柄（由总线层下发，驱动层只持有不释放） */
typedef i2c_master_dev_handle_t bsp_i2c_dev_t;

/** 总线统计（诊断用，只读） */
typedef struct
{
    uint32_t tx;            /*!< 写事务次数 */
    uint32_t rx;            /*!< 读/写读事务次数 */
    uint32_t errors;        /*!< 失败次数 */
    esp_err_t last_error;   /*!< 最近一次错误码 */
} bsp_i2c_stats_t;

/**
 * @brief  初始化 I2C0 主机总线（幂等）
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief  释放 I2C0 主机总线（需要先卸载全部从设备）
 */
esp_err_t bsp_i2c_deinit(void);

/**
 * @brief  在总线上挂载一个从设备
 * @param  addr     7bit 从地址
 * @param  scl_hz   该设备使用的 SCL 频率（0 = 使用总线默认 400kHz）
 * @param  out_dev  输出设备句柄
 */
esp_err_t bsp_i2c_add_device(uint16_t addr, uint32_t scl_hz, bsp_i2c_dev_t *out_dev);

/**
 * @brief  卸载从设备
 */
esp_err_t bsp_i2c_rm_device(bsp_i2c_dev_t dev);

/**
 * @brief  写数据（单次事务）
 */
esp_err_t bsp_i2c_write(bsp_i2c_dev_t dev, const uint8_t *buf, size_t len);

/**
 * @brief  读数据（单次事务）
 */
esp_err_t bsp_i2c_read(bsp_i2c_dev_t dev, uint8_t *buf, size_t len);

/**
 * @brief  写一段再读一段（同一事务内，带重复起始）
 */
esp_err_t bsp_i2c_write_read(bsp_i2c_dev_t dev,
                             const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len);

/**
 * @brief  写寄存器（reg + 数据，一次事务）
 */
esp_err_t bsp_i2c_reg_write(bsp_i2c_dev_t dev, uint8_t reg, const uint8_t *val, size_t len);

/**
 * @brief  读寄存器（写 reg + 重复起始 + 读数据，一次事务）
 */
esp_err_t bsp_i2c_reg_read(bsp_i2c_dev_t dev, uint8_t reg, uint8_t *val, size_t len);

/**
 * @brief  写 8bit 寄存器（最常用的快捷形式）
 */
esp_err_t bsp_i2c_reg_write_u8(bsp_i2c_dev_t dev, uint8_t reg, uint8_t val);

/**
 * @brief  读 8bit 寄存器
 */
esp_err_t bsp_i2c_reg_read_u8(bsp_i2c_dev_t dev, uint8_t reg, uint8_t *val);

/**
 * @brief  探测某个 7bit 地址是否有器件应答
 * @return true 有 ACK
 * @note   也用于 EEPROM 的"写周期探测"：器件在内部写周期内不会 ACK。
 */
bool bsp_i2c_probe(uint16_t addr, uint32_t timeout_ms);

/**
 * @brief  获取总线统计
 */
void bsp_i2c_get_stats(bsp_i2c_stats_t *out);

/**
 * @brief  清除总线统计
 */
void bsp_i2c_clear_stats(void);

/**
 * @brief  加锁 / 解锁（可重入）
 * @note   用于"必须连续完成、中间不能被其它任务插队"的复合时序，
 *         例如 EEPROM 跨页写、触摸寄存器 4 字节地址 + 读 28 字节。
 */
esp_err_t bsp_i2c_lock(uint32_t timeout_ms);
esp_err_t bsp_i2c_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_I2C_BUS_H */
