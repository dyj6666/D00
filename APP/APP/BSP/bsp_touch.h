#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H

#include <stdint.h>

/* ================================================================
 * 电阻触摸屏 BSP 层（XPT2046 / ADS7843 兼容，位操作 SPI）
 *   引脚（探索者V3 LCD 触摸接口）：
 *     T_CLK = PB0   T_PEN = PB1   T_MISO = PB2
 *     T_CS  = PC13  T_MOSI = PF11
 *   性能：DWT 精确半时钟（~1.6MHz SPI），5 次采样去极值均值 +
 *         双次校验（±50），单点采样 ~280us（>3k 次/秒）。
 *   校准：线性模型  逻辑坐标 = (物理AD - 中心) / 比例 + 屏尺寸/2
 * ================================================================ */

/* ---------- 校准参数 ---------- */
typedef struct {
    int32_t xfac;       /* X 比例：物理AD单位/像素（>0） */
    int32_t yfac;       /* Y 比例 */
    int32_t xc;         /* X 物理中心 */
    int32_t yc;         /* Y 物理中心 */
    uint8_t valid;      /* 1=已校准（默认/运行时校准） */
} bsp_touch_cal_t;

/* ---------- 接口 ---------- */

/* 初始化 GPIO（上拉输入 PEN/MISO，推挽输出 CLK/MOSI/CS） */
void BSP_Touch_Init(void);

/* PEN 引脚：1=有触摸 */
uint8_t BSP_Touch_Pressed(void);

/* 读取滤波后的物理坐标（5 次去极值均值），0=读数无效 */
uint8_t BSP_Touch_ReadRaw(int32_t *rx, int32_t *ry);

/* 无条件完整读取（诊断/校准用，不判触点范围） */
uint8_t BSP_Touch_ReadRawForce(int32_t *rx, int32_t *ry);

/* 物理坐标 → 逻辑坐标（LCD 像素），越界钳位 */
void BSP_Touch_Convert(int32_t rx, int32_t ry, uint16_t *lx, uint16_t *ly);

/* 校准参数读写（内存态；持久化由上层决定） */
void BSP_Touch_SetCal(const bsp_touch_cal_t *cal);
void BSP_Touch_GetCal(bsp_touch_cal_t *cal);

#endif
