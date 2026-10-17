/* ================================================================
 * bsp.h —— 板级支持包总入口
 *
 * 典型用法：
 *   // 1) 全量初始化（内部自动排序 + 自动补齐依赖）
 *   ESP_ERROR_CHECK(bsp_init(BSP_MOD_ALL));
 *   bsp_print_info();                       // 打印板级信息与 I2C 扫描结果
 *
 *   // 2) 只初始化需要的子系统（依赖会被自动带上）
 *   ESP_ERROR_CHECK(bsp_init(BSP_MOD_LCD | BSP_MOD_IMU));
 *
 *   // 3) 模块级 API
 *   bsp_lcd_fill(0x0000);
 *   bsp_led_set_brightness(50);
 *
 * 设计约束：
 *   - 所有 init 幂等且可回滚：某模块失败时，已启动的模块按相反顺序回滚；
 *   - 单例模型：每种外设一份（板载外设本就唯一）；
 *   - 线程安全：共享总线内部加锁；各设备驱动的例外会在其头文件里标注。
 * ================================================================ */
#ifndef __BSP_H
#define __BSP_H

#include "bsp_types.h"
#include "bsp_pinmap.h"      /* 顺手带上引脚表：应用只 include bsp.h 也能拿到 BSP_LCD_* 等常量 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 版本与板卡信息 ---------------- */

#define BSP_VERSION_MAJOR       1
#define BSP_VERSION_MINOR       0
#define BSP_VERSION_PATCH       0

/**
 * @brief  板卡名称
 */
const char *bsp_board_name(void);

/**
 * @brief  BSP 版本字符串（如 "1.0.0"）
 */
const char *bsp_version_string(void);

/* ---------------- 生命周期 ---------------- */

/**
 * @brief  按依赖顺序初始化指定模块
 * @param  modules 模块掩码，常用 BSP_MOD_ALL
 * @return ESP_OK 成功；否则为第一个失败模块的错误码
 *
 * @note   1. 重复调用安全：已在运行的模块会被跳过；
 *         2. 依赖自动补齐（见 bsp.c 的依赖表）；
 *         3. 任一步失败会回滚本次已启动的模块，不留半初始化状态。
 */
esp_err_t bsp_init(bsp_module_mask_t modules);

/**
 * @brief  反初始化：按初始化相反顺序释放全部已启动模块
 * @return ESP_OK（单个模块释放失败只记日志，不阻断后续释放）
 */
esp_err_t bsp_deinit(void);

/**
 * @brief  查询某模块是否已初始化
 */
bool bsp_module_is_running(bsp_module_t module);

/**
 * @brief  获取当前已初始化的模块掩码
 */
bsp_module_mask_t bsp_modules_running(void);

/* ---------------- 自检与诊断 ---------------- */

/**
 * @brief  打印板级信息：模块状态、关键引脚分配、I2C 总线扫描
 * @note   只读操作，不影响业务；建议开机调用一次
 */
void bsp_print_info(void);

/**
 * @brief  扫描 I2C0 总线上所有应答的 7bit 地址
 * @param  found   输出数组，存放应答到的地址
 * @param  max     数组容量
 * @return 实际探测到的设备个数（< 0 表示总线不可用）
 */
int bsp_i2c_scan(uint8_t *found, size_t max);

/**
 * @brief  只读自检：检查板上器件是否在线
 * @param  report  非 0 时打印每个器件的探测结果
 * @return ESP_OK 表示所有"应当存在"的器件都应答了
 *
 * @note   只做探测（I2C ACK / WHO_AM_I 读回），不写寄存器、不改变器件状态。
 */
esp_err_t bsp_selftest(bool report);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_H */
