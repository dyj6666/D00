/* ================================================================
 * key.c —— 按键驱动（BOOT + KEY0 + KEY1）
 *
 * 中断来源有两条，都只负责"叫醒任务"，不做任何 I2C：
 *   1. BOOT(IO0) 的 GPIO 下降沿；
 *   2. XL9555 的 INT(IO3) 下降沿（KEY0/KEY1 变化会拉低它），
 *      经 bsp_xl9555_attach_int() 注册的 ISR 回调转成任务通知。
 *
 * 注意：XL9555 的 INT 是"任一输入变化"都会触发（触摸中断、传感器中断、
 *       扩展 IO 都会拉低它），所以任务被唤醒后要用实际电平过滤，
 *       只在 KEY0/KEY1 真的变了才产生事件。
 * ================================================================ */
#include "bsp/key.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/xl9555.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/KEY";

#define KEY_TASK_STACK          3072U
#define KEY_TASK_PRIO           4U

/** 一个按键的状态机 */
typedef struct
{
    bool     raw_pressed;       /*!< 最近一次读到的原始电平（已折算成"按下"语义） */
    bool     stable_pressed;    /*!< 消抖后的稳定状态 */
    uint32_t raw_change_ms;     /*!< 原始电平最近一次变化的时刻 */
    uint32_t down_ms;           /*!< 消抖确认按下的时刻 */
    bool     long_fired;        /*!< 长按事件是否已发过 */
} key_state_t;

static bsp_key_cfg_t     s_cfg = BSP_KEY_CFG_DEFAULT;
static bsp_key_cb_t      s_cb = NULL;
static void             *s_cb_ctx = NULL;
static TaskHandle_t      s_task = NULL;
static key_state_t       s_state[BSP_KEY_COUNT];
static bool              s_ready = false;

/* ---------------- 工具 ---------------- */

/**
 * @brief  当前时刻（毫秒）
 */
static inline uint32_t key_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/**
 * @brief  读取某个键的原始"按下"状态
 */
static bool key_read_raw(bsp_key_id_t key)
{
    if (key == BSP_KEY_BOOT) {
        return gpio_get_level(BSP_KEY_BOOT_GPIO) == 0;      /* 低电平有效 */
    }

    uint16_t inputs = 0;

    if (bsp_xl9555_inputs(&inputs) != ESP_OK) {
        return s_state[key].stable_pressed;                 /* 读失败：维持原状态 */
    }

    const uint16_t pin = (key == BSP_KEY_0) ? BSP_XL9555_PIN_KEY0 : BSP_XL9555_PIN_KEY1;

    return (inputs & pin) == 0U;                            /* 低电平有效 */
}

/**
 * @brief  派发事件
 */
static void key_dispatch(bsp_key_id_t key, bsp_key_event_t event, uint32_t hold_ms)
{
    if (s_cb != NULL) {
        s_cb(key, event, hold_ms, s_cb_ctx);
    }
}

/**
 * @brief  扫描一遍所有按键并推进状态机
 * @return 是否还有键处于"活动"状态（按下或等待消抖）
 */
static bool key_scan_once(void)
{
    const uint32_t now = key_now_ms();
    bool active = false;

    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        key_state_t *st = &s_state[i];
        const bool raw = key_read_raw((bsp_key_id_t)i);

        /* ① 原始电平变化：记录时刻（消抖起点） */
        if (raw != st->raw_pressed) {
            st->raw_pressed = raw;
            st->raw_change_ms = now;
            active = true;
            continue;
        }

        /* ② 稳定状态需要等消抖时间 */
        if (raw != st->stable_pressed && (now - st->raw_change_ms) >= s_cfg.debounce_ms) {
            st->stable_pressed = raw;

            if (raw) {
                st->down_ms = now;
                st->long_fired = false;
                key_dispatch((bsp_key_id_t)i, BSP_KEY_EVENT_DOWN, 0U);
            } else {
                const uint32_t hold = now - st->down_ms;

                key_dispatch((bsp_key_id_t)i, BSP_KEY_EVENT_UP, hold);
                if (hold < s_cfg.long_press_ms) {
                    key_dispatch((bsp_key_id_t)i, BSP_KEY_EVENT_CLICK, hold);
                }
            }
        }

        /* ③ 长按检测（按住期间每拍检查一次） */
        if (st->stable_pressed && !st->long_fired) {
            const uint32_t hold = now - st->down_ms;

            if (hold >= s_cfg.long_press_ms) {
                st->long_fired = true;
                key_dispatch((bsp_key_id_t)i, BSP_KEY_EVENT_LONG_PRESS, hold);
            }
        }

        if (st->stable_pressed || raw != st->stable_pressed) {
            active = true;
        }
    }

    return active;
}

/* ---------------- 中断 ---------------- */

/**
 * @brief  BOOT 键 GPIO 中断（ISR）
 */
static void IRAM_ATTR key_boot_isr(void *arg)
{
    BSP_UNUSED(arg);

    BaseType_t woken = pdFALSE;

    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief  XL9555 中断回调（ISR 上下文）
 */
static void key_xl9555_isr(void *ctx)
{
    BSP_UNUSED(ctx);

    BaseType_t woken = pdFALSE;

    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------- 任务 ---------------- */

/**
 * @brief  按键扫描任务：平时阻塞在通知上，有键按下才按节拍扫描
 */
static void key_task(void *arg)
{
    BSP_UNUSED(arg);

    for (;;) {
        const bool active = key_scan_once();
        const TickType_t wait = active ? pdMS_TO_TICKS(s_cfg.poll_period_ms) : portMAX_DELAY;

        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t bsp_key_configure(const bsp_key_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;           /* 运行中不允许改 */
    }

    s_cfg = *cfg;
    if (s_cfg.debounce_ms == 0U) {
        s_cfg.debounce_ms = 20U;
    }
    if (s_cfg.long_press_ms == 0U) {
        s_cfg.long_press_ms = 1000U;
    }
    if (s_cfg.poll_period_ms == 0U) {
        s_cfg.poll_period_ms = 10U;
    }
    return ESP_OK;
}

esp_err_t bsp_key_set_callback(bsp_key_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t bsp_key_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ① BOOT 键：输入 + 上拉 + 下降沿中断 */
    const gpio_config_t boot_cfg = {
        .pin_bit_mask = BSP_BIT(BSP_KEY_BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&boot_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        s_state[i].raw_pressed = false;
        s_state[i].stable_pressed = false;
        s_state[i].raw_change_ms = 0;
        s_state[i].down_ms = 0;
        s_state[i].long_fired = false;
    }

    /* ② 先建任务，再挂中断，避免中断打到空任务句柄 */
    if (xTaskCreate(key_task, "bsp_key", KEY_TASK_STACK, NULL, KEY_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
        return ESP_ERR_NO_MEM;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "install isr service failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = gpio_isr_handler_add(BSP_KEY_BOOT_GPIO, key_boot_isr, NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot isr add failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* ③ XL9555 中断：ISR 回调只做任务通知 */
    err = bsp_xl9555_attach_int(key_xl9555_isr, NULL);
    if (err == ESP_OK) {
        err = bsp_xl9555_int_enable(true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xl9555 int enable failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (BOOT=IO%d, KEY0/KEY1=XL9555, debounce=%ums long=%ums)",
             (int)BSP_KEY_BOOT_GPIO, (unsigned)s_cfg.debounce_ms, (unsigned)s_cfg.long_press_ms);
    return ESP_OK;

fail:
    (void)gpio_isr_handler_remove(BSP_KEY_BOOT_GPIO);
    vTaskDelete(s_task);
    s_task = NULL;
    return err;
}

esp_err_t bsp_key_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    (void)bsp_xl9555_detach_int();
    (void)bsp_xl9555_int_enable(false);
    (void)gpio_isr_handler_remove(BSP_KEY_BOOT_GPIO);

    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    s_ready = false;
    return ESP_OK;
}

bool bsp_key_is_down(bsp_key_id_t key)
{
    if (key >= BSP_KEY_COUNT) {
        return false;
    }
    return s_state[key].stable_pressed;
}

uint32_t bsp_key_hold_ms(bsp_key_id_t key)
{
    if (key >= BSP_KEY_COUNT || !s_state[key].stable_pressed) {
        return 0U;
    }
    return key_now_ms() - s_state[key].down_ms;
}

esp_err_t bsp_key_get(bsp_key_id_t key, bool *down, uint32_t *hold_ms)
{
    if (key >= BSP_KEY_COUNT || down == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *down = s_state[key].stable_pressed;
    if (hold_ms != NULL) {
        *hold_ms = (*down) ? (key_now_ms() - s_state[key].down_ms) : 0U;
    }
    return ESP_OK;
}
