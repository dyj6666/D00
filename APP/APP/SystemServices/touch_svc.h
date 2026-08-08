#ifndef TOUCH_SVC_H
#define TOUCH_SVC_H

#include <stdint.h>

/* ================================================================
 * 触摸服务（系统输入层）
 *   - 独立采样任务：PEN 轮询（空闲 100Hz）+ 按下后 1kHz 高速采样
 *   - 状态机：UP → DOWN → MOVE → UP（含轻平滑）
 *   - 共享状态 + 代数计数器：UI 框架轮询消费，无队列洪泛
 *   - 校准：TouchSvc_Calibrate() 四角校准（内存态，默认值可用）
 * ================================================================ */

typedef enum {
    TOUCH_EVT_NONE = 0,
    TOUCH_EVT_DOWN,
    TOUCH_EVT_MOVE,
    TOUCH_EVT_UP,
    TOUCH_EVT_TAP,      /* UI 层合成的轻点事件（非滑动释放时） */
} touch_evt_t;

typedef struct {
    volatile uint16_t x, y;         /* 逻辑坐标（LCD 像素） */
    volatile uint16_t raw_x, raw_y; /* 物理 AD（校准用） */
    volatile uint8_t  state;        /* touch_evt_t */
    volatile uint16_t down_x, down_y;   /* 按下起点 */
    volatile uint16_t up_x, up_y;       /* 抬起终点 */
    volatile uint16_t max_dx, max_dy;   /* 触摸全程最大位移（手势判定用） */
    volatile uint32_t gen;              /* 每次变化 +1（UI 轮询用） */
} touch_svc_state_t;

/* 初始化：启动采样任务（应用初始化时调用一次） */
void TouchSvc_Init(void);

/* 获取共享状态指针（UI 框架轮询） */
const touch_svc_state_t *TouchSvc_GetState(void);

/* 四角校准（阻塞执行，调用方为非渲染任务）：
 *   依次在屏幕 4 个十字处按压，采集物理 AD 建立线性映射。 */
void TouchSvc_Calibrate(void);

#endif
