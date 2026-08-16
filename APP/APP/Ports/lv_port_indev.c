/* ================================================================
 * lv_port_indev —— LVGL 输入端口实现（电阻触摸 → LVGL 指针设备）
 *
 * 架构位置：APP Ports 层；轮询 touch_svc 共享状态（逻辑像素坐标），
 *           DOWN/MOVE 视为按下，其余视为释放；不感知手势语义。
 * ================================================================ */
#include "lvgl.h"
#include "lv_port_indev.h"
#include "touch_svc.h"
#include "bsp_gpio.h"

static lv_indev_drv_t s_indev_drv;
static lv_indev_t *s_indev;

static lv_indev_drv_t s_key_drv;
static lv_indev_t *s_key_indev;

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

/* ---------------- LVGL keypad 读取回调（KEY0 单键桥接） ----------------
 * 按下沿发 ENTER（确认），按住 1s 发一次 ESC（返回）。
 * 应用层页面导航（短按翻页/长按回主页）由 gui_app 订阅事件总线完成；
 * 本设备把按键桥接进 LVGL 输入系统，供可聚焦控件（按钮/列表/滑块）
 * 直接使用，与触摸指针设备并存互不干扰。 */
#define KEY_LONG_PRESS_MS   1000u

static void indev_keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static uint8_t   was_pressed = 0;
    static uint32_t  press_start = 0;
    static uint8_t   long_sent = 0;

    uint8_t pressed = BSP_KeyPressed() ? 1u : 0u;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->key = 0;   /* 默认无键值（LV_KEY_NONE 不存在，0 即忽略） */

    if (pressed) {
        if (!was_pressed) {
            press_start = lv_tick_get();
            long_sent = 0;
            data->key = LV_KEY_ENTER;   /* 按下沿：确认 */
        } else if (!long_sent && (lv_tick_get() - press_start) > KEY_LONG_PRESS_MS) {
            data->key = LV_KEY_ESC;     /* 长按 1s：返回（仅发一次） */
            long_sent = 1;
        }
    }
    was_pressed = pressed;
}

/* ---------------- 输入端口初始化 ---------------- */
void LvPort_IndevInit(void)
{
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    (void)s_indev;   /* 首个输入设备自动生效，句柄保留供后续扩展 */

    /* keypad：KEY0 桥接为 ENTER/ESC（未来可聚焦控件直接可用） */
    lv_indev_drv_init(&s_key_drv);
    s_key_drv.type    = LV_INDEV_TYPE_KEYPAD;
    s_key_drv.read_cb = indev_keypad_read;
    s_key_indev = lv_indev_drv_register(&s_key_drv);
    (void)s_key_indev;
}
