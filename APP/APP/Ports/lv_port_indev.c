/* ================================================================
 * lv_port_indev —— LVGL 输入端口实现（电阻触摸 → LVGL 指针设备）
 *
 * 架构位置：APP Ports 层；轮询 touch_svc 共享状态（逻辑像素坐标），
 *           DOWN/MOVE 视为按下，其余视为释放；不感知手势语义。
 * ================================================================ */
#include "lvgl.h"
#include "lv_port_indev.h"
#include "touch_svc.h"

static lv_indev_drv_t s_indev_drv;
static lv_indev_t *s_indev;

/* ---------------- LVGL 指针读取回调 ---------------- */
static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    const touch_svc_state_t *ts = TouchSvc_GetState();

    data->point.x = ts->x;
    data->point.y = ts->y;
    /* 物理触摸过程中为按下：DOWN/MOVE；NONE/UP/TAP 视为释放 */
    data->state = (ts->state == TOUCH_EVT_DOWN || ts->state == TOUCH_EVT_MOVE)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

/* ---------------- 输入端口初始化 ---------------- */
void LvPort_IndevInit(void)
{
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    (void)s_indev;   /* 首个输入设备自动生效，句柄保留供后续扩展 */
}
