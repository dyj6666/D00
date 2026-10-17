/* ================================================================
 * beep.c —— 板载有源蜂鸣器
 *
 * 实现要点：
 *   1. P0_2 是"高 = 关"，所以写寄存器时要取反，避免上电就响；
 *   2. 用一次性/周期性 esp_timer 驱动，调用方不必阻塞等待；
 *   3. 与 XL9555 的其它位共用同一条 I2C 事务路径（掩码写），互不干扰。
 * ================================================================ */
#include "bsp/beep.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/xl9555.h"

#include "esp_timer.h"

static const char *TAG = "BSP/BEEP";

#define BEEP_MAX_TIMES          10U

static bool               s_ready = false;
static bool               s_on = false;
static esp_timer_handle_t s_oneshot = NULL;
static esp_timer_handle_t s_pattern = NULL;
static uint8_t            s_pattern_left = 0;
static uint32_t           s_pattern_on_ms = 0;
static uint32_t           s_pattern_off_ms = 0;
static bool               s_pattern_phase_on = false;

/* ---------------- 内部 ---------------- */

/**
 * @brief  真正驱动蜂鸣器（高 = 关，故取反）
 */
static esp_err_t beep_apply(bool on)
{
    return bsp_xl9555_pin_write(BSP_XL9555_PIN_BEEP, !on);
}

/**
 * @brief  一次性定时器到期：静音
 */
static void beep_oneshot_cb(void *arg)
{
    BSP_UNUSED(arg);
    (void)beep_apply(false);
    s_on = false;
}

/**
 * @brief  序列定时器：ON/OFF 交替，响够次数后自动静音
 * @note   状态机很直白：s_pattern_phase_on 表示"当前正处于响的阶段"，
 *         定时器到期就切到另一阶段并重装；每结束一段"响"就扣一次计数，
 *         剩 0 时不再重装，于是停在静音。
 */
static void beep_pattern_cb(void *arg)
{
    BSP_UNUSED(arg);

    if (s_pattern_phase_on) {
        (void)beep_apply(false);
        s_on = false;
        s_pattern_phase_on = false;

        if (s_pattern_left > 0U) {
            s_pattern_left--;
        }
        if (s_pattern_left > 0U) {
            (void)esp_timer_start_once(s_pattern, (uint64_t)s_pattern_off_ms * 1000ULL);
        }
    } else {
        (void)beep_apply(true);
        s_on = true;
        s_pattern_phase_on = true;
        (void)esp_timer_start_once(s_pattern, (uint64_t)s_pattern_on_ms * 1000ULL);
    }
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_beep_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const esp_timer_create_args_t oneshot_args = {
        .callback = beep_oneshot_cb,
        .name = "beep_once",
    };
    esp_err_t err = esp_timer_create(&oneshot_args, &s_oneshot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create oneshot timer failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t pattern_args = {
        .callback = beep_pattern_cb,
        .name = "beep_pattern",
    };
    err = esp_timer_create(&pattern_args, &s_pattern);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create pattern timer failed: %s", esp_err_to_name(err));
        return err;
    }

    err = beep_apply(false);                    /* 确保静音 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mute failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (XL9555 P0_2, high=off)");
    return ESP_OK;
}

esp_err_t bsp_beep_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    (void)bsp_beep_stop();
    if (s_oneshot != NULL) {
        (void)esp_timer_delete(s_oneshot);
        s_oneshot = NULL;
    }
    if (s_pattern != NULL) {
        (void)esp_timer_delete(s_pattern);
        s_pattern = NULL;
    }
    s_ready = false;
    return ESP_OK;
}

/* ---------------- 控制 ---------------- */

esp_err_t bsp_beep_set(bool on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)esp_timer_stop(s_oneshot);
    (void)esp_timer_stop(s_pattern);

    esp_err_t err = beep_apply(on);
    if (err == ESP_OK) {
        s_on = on;
    }
    return err;
}

bool bsp_beep_is_on(void)
{
    return s_on;
}

esp_err_t bsp_beep_once(uint32_t ms)
{
    if (!s_ready || ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)esp_timer_stop(s_oneshot);
    (void)esp_timer_stop(s_pattern);

    esp_err_t err = beep_apply(true);
    if (err != ESP_OK) {
        return err;
    }
    s_on = true;

    err = esp_timer_start_once(s_oneshot, (uint64_t)ms * 1000ULL);
    if (err != ESP_OK) {
        (void)beep_apply(false);
        s_on = false;
    }
    return err;
}

esp_err_t bsp_beep_beep(uint8_t times, uint32_t on_ms, uint32_t off_ms)
{
    if (!s_ready || times == 0U || times > BEEP_MAX_TIMES || on_ms == 0U || off_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)esp_timer_stop(s_oneshot);
    (void)esp_timer_stop(s_pattern);

    s_pattern_left = times;             /* 剩余次数（含当前这一声） */
    s_pattern_on_ms = on_ms;
    s_pattern_off_ms = off_ms;
    s_pattern_phase_on = true;          /* 当前处于"响"的阶段 */

    esp_err_t err = beep_apply(true);
    if (err != ESP_OK) {
        return err;
    }
    s_on = true;

    err = esp_timer_start_once(s_pattern, (uint64_t)on_ms * 1000ULL);
    if (err != ESP_OK) {
        (void)beep_apply(false);
        s_on = false;
        s_pattern_phase_on = false;
    }
    return err;
}

esp_err_t bsp_beep_stop(void)
{
    if (s_oneshot != NULL) {
        (void)esp_timer_stop(s_oneshot);
    }
    if (s_pattern != NULL) {
        (void)esp_timer_stop(s_pattern);
    }
    s_pattern_left = 0;

    if (s_ready) {
        esp_err_t err = beep_apply(false);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_on = false;
    return ESP_OK;
}
