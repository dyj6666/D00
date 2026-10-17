/* ================================================================
 * led.c —— 板载 LED 驱动
 *
 * 极性说明（来自厂商例程 led.c 的实测行为）：
 *   led_init() 里 LED(1) 注释是"关闭 LED" -> IO4 高电平 = 灭，即**低电平点亮**。
 *   因此 LEDC 通道开 output_invert，让"占空比 = 亮度"这个直觉成立；
 *   红灯同理（XL9555 写 0 才亮）。
 * ================================================================ */
#include "bsp/led.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/xl9555.h"

#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "BSP/LED";

#define LED_PWM_MODE            LEDC_LOW_SPEED_MODE
#define LED_PWM_TIMER           LEDC_TIMER_0
#define LED_PWM_CHANNEL         LEDC_CHANNEL_0
#define LED_PWM_RES             LEDC_TIMER_10_BIT
#define LED_PWM_FREQ_HZ         5000U
#define LED_PWM_FULL            ((1U << 10) - 1U)   /* 10bit 满量程 */

static bool             s_ready = false;
static bool             s_blue_on = false;
static bool             s_red_on = false;
static uint8_t          s_blue_percent = 0;
static esp_timer_handle_t s_breath_timer = NULL;
static bool             s_breath_up = true;
static uint32_t         s_breath_half_ms = 1000U;
static esp_timer_handle_t s_blink_timer[BSP_LED_COUNT] = {NULL, NULL};

/* ---------------- 内部工具 ---------------- */

/**
 * @brief  亮度百分比 -> LEDC 占空比
 */
static uint32_t led_percent_to_duty(uint8_t percent)
{
    const uint32_t p = (uint32_t)BSP_CLAMP(percent, 0U, 100U);

    return (p * LED_PWM_FULL) / 100U;
}

/**
 * @brief  真正驱动红灯（低电平点亮）
 */
static esp_err_t led_red_apply(bool on)
{
    return bsp_xl9555_pin_write(BSP_XL9555_PIN_LED_R, !on);
}

/**
 * @brief  呼吸定时器回调：翻转硬件渐变目标
 */
static void led_breath_cb(void *arg)
{
    BSP_UNUSED(arg);

    const uint8_t target = s_breath_up ? 100U : 0U;

    s_breath_up = !s_breath_up;
    (void)bsp_led_fade_to(target, s_breath_half_ms);
}

/**
 * @brief  闪烁定时器回调
 */
static void led_blink_cb(void *arg)
{
    bsp_led_t led = (bsp_led_t)(uintptr_t)arg;

    (void)bsp_led_toggle(led);
}

/**
 * @brief  停止某个 LED 的闪烁定时器
 */
static void led_blink_stop_timer(bsp_led_t led)
{
    if (led < BSP_LED_COUNT && s_blink_timer[led] != NULL) {
        (void)esp_timer_stop(s_blink_timer[led]);
    }
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_led_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ① 蓝色 LED：LEDC 硬件 PWM，输出反相以匹配低电平点亮 */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LED_PWM_MODE,
        .duty_resolution = LED_PWM_RES,
        .timer_num       = LED_PWM_TIMER,
        .freq_hz         = LED_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t ch_cfg = {
        .gpio_num   = (int)BSP_LED_BLUE_GPIO,
        .speed_mode = LED_PWM_MODE,
        .channel    = LED_PWM_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LED_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = 1,               /* 低电平点亮 -> 反相 */
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ② 硬件渐变功能（呼吸灯要用），已安装过则忽略错误 */
    err = ledc_fade_func_install(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "ledc fade install: %s", esp_err_to_name(err));
    }

    /* ③ 红灯：IO 扩展上的开关量，起始灭 */
    err = led_red_apply(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "red led init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ④ 两个闪烁定时器（按需启动） */
    for (int i = 0; i < BSP_LED_COUNT; i++) {
        if (s_blink_timer[i] == NULL) {
            const esp_timer_create_args_t targs = {
                .callback = led_blink_cb,
                .arg = (void *)(uintptr_t)i,
                .name = "led_blink",
            };
            err = esp_timer_create(&targs, &s_blink_timer[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "create blink timer failed: %s", esp_err_to_name(err));
                return err;
            }
        }
    }

    s_blue_percent = 0;
    s_ready = true;
    ESP_LOGI(TAG, "ready (blue=PWM%dHz on IO%d, red=XL9555 P1_0)",
             (int)LED_PWM_FREQ_HZ, (int)BSP_LED_BLUE_GPIO);
    return ESP_OK;
}

esp_err_t bsp_led_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    (void)bsp_led_breath_stop();

    for (int i = 0; i < BSP_LED_COUNT; i++) {
        led_blink_stop_timer((bsp_led_t)i);
    }

    (void)bsp_led_set(BSP_LED_BLUE, false);
    (void)bsp_led_set(BSP_LED_RED, false);

    esp_err_t err = ledc_stop(LED_PWM_MODE, LED_PWM_CHANNEL, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ledc stop: %s", esp_err_to_name(err));
    }

    ledc_fade_func_uninstall();
    s_ready = false;
    return ESP_OK;
}

/* ---------------- 基本控制 ---------------- */

esp_err_t bsp_led_set(bsp_led_t led, bool on)
{
    if (!s_ready || led >= BSP_LED_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    if (led == BSP_LED_BLUE) {
        esp_err_t err = bsp_led_fade_to(on ? 100U : 0U, 0U);
        if (err == ESP_OK) {
            s_blue_on = on;
        }
        return err;
    }

    esp_err_t err = led_red_apply(on);
    if (err == ESP_OK) {
        s_red_on = on;
    }
    return err;
}

esp_err_t bsp_led_toggle(bsp_led_t led)
{
    if (led >= BSP_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_led_set(led, !bsp_led_is_on(led));
}

bool bsp_led_is_on(bsp_led_t led)
{
    if (led == BSP_LED_BLUE) {
        return s_blue_on;
    }
    if (led == BSP_LED_RED) {
        return s_red_on;
    }
    return false;
}

/* ---------------- 亮度与渐变（蓝灯）---------------- */

esp_err_t bsp_led_fade_to(uint8_t percent, uint32_t ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t p = (uint8_t)BSP_CLAMP(percent, 0U, 100U);
    const uint32_t duty = led_percent_to_duty(p);

    esp_err_t err;

    if (ms == 0U) {
        err = ledc_set_duty(LED_PWM_MODE, LED_PWM_CHANNEL, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(LED_PWM_MODE, LED_PWM_CHANNEL);
        }
    } else {
        err = ledc_set_fade_with_time(LED_PWM_MODE, LED_PWM_CHANNEL, duty, (int)ms);
        if (err == ESP_OK) {
            err = ledc_fade_start(LED_PWM_MODE, LED_PWM_CHANNEL, LEDC_FADE_NO_WAIT);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fade to %u%% failed: %s", (unsigned)p, esp_err_to_name(err));
        return err;
    }

    s_blue_percent = p;
    s_blue_on = (p > 0U);
    return ESP_OK;
}

esp_err_t bsp_led_set_brightness(uint8_t percent)
{
    return bsp_led_fade_to(percent, 0U);
}

uint8_t bsp_led_get_brightness(void)
{
    return s_blue_percent;
}

/* ---------------- 呼吸灯 ---------------- */

esp_err_t bsp_led_breath_start(uint32_t half_period_ms)
{
    if (!s_ready || half_period_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_breath_half_ms = half_period_ms;
    s_breath_up = true;

    if (s_breath_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = led_breath_cb,
            .name = "led_breath",
        };
        esp_err_t err = esp_timer_create(&targs, &s_breath_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "create breath timer failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    (void)esp_timer_stop(s_breath_timer);
    return esp_timer_start_periodic(s_breath_timer, (uint64_t)half_period_ms * 1000ULL);
}

esp_err_t bsp_led_breath_stop(void)
{
    if (s_breath_timer != NULL) {
        (void)esp_timer_stop(s_breath_timer);
    }
    return ESP_OK;
}

/* ---------------- 闪烁 ---------------- */

esp_err_t bsp_led_blink_start(bsp_led_t led, uint32_t period_ms)
{
    if (!s_ready || led >= BSP_LED_COUNT || period_ms < 20U) {
        return ESP_ERR_INVALID_ARG;
    }

    led_blink_stop_timer(led);
    (void)bsp_led_set(led, true);
    return esp_timer_start_periodic(s_blink_timer[led], (uint64_t)period_ms * 500ULL);
}

esp_err_t bsp_led_blink_stop(bsp_led_t led)
{
    if (led >= BSP_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    led_blink_stop_timer(led);
    return bsp_led_set(led, false);
}
