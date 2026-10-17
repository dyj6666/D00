/* ================================================================
 * main —— ESP32-S3-BOX（正点原子 AI BOX1）环境自检固件（bringup）
 *
 * 架构位置：ESP 子空间第一个工程；一次烧录验证整条工具链与板级基础能力
 *
 * 自检项：
 *   ① 芯片信息（型号 / 核心数 / 修订 / 特性位）
 *   ② Flash 容量（N16R8 → 应为 16MB）
 *   ③ PSRAM 容量（Octal 模式 → 应为 8MB）
 *   ④ 出厂 MAC（WiFi STA）
 *   ⑤ 复位原因（区分上电 / 看门狗 / 软件复位等）
 *   ⑥ NVS 初始化（后续 WiFi/BLE 配网的存储依赖）
 *   ⑦ FreeRTOS 心跳任务（验证调度、堆水位健康）
 *
 * 硬件：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB Octal PSRAM，240MHz）
 * 工具链：ESP-IDF v5.2.1
 * ================================================================ */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "bsp/bsp.h"

#include "boot_ui.h"

/* ---------------- 常量 ---------------- */
static const char *TAG = "BRINGUP";

#define HEARTBEAT_PERIOD_MS  5000   /* 心跳周期（验证调度与堆健康） */

/* ================================================================
 *                        辅助函数
 * ================================================================ */

/**
 * @brief  复位原因 → 可读字符串（诊断启动链路问题用）
 * @param  r  esp_reset_reason() 的返回值
 * @return  静态字符串（无需释放）
 */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON (上电/复位键)";
    case ESP_RST_EXT:       return "EXT (外部复位脚)";
    case ESP_RST_SW:        return "SW (软件 esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (异常/abort)";
    case ESP_RST_INT_WDT:   return "INT_WDT (中断看门狗)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (任务看门狗)";
    case ESP_RST_WDT:       return "WDT (其他看门狗)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (深睡唤醒)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (欠压)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB (USB 外设复位)";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";   /* IDF 5.2 无 EFUSE/PWR_GLITCH/CPU_LOCKUP 枚举 */
    }
}

/**
 * @brief  芯片型号 → 可读字符串
 * @param  model  esp_chip_info_t.model
 * @return  静态字符串
 */
static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "UNKNOWN";
    }
}

/**
 * @brief  打印芯片 / 存储 / 网络 / 启动链路自检信息
 * @note   全部使用英文日志（串口监视器对中文支持不佳，避免乱码）
 */
static void print_system_info(void)
{
    /* ---- ① 芯片信息 ---- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "========== ESP32-S3-BOX (AI BOX1) BRINGUP ==========");
    ESP_LOGI(TAG, "[CHIP ] model=%s cores=%d revision=v%d.%d",
             chip_model_str(chip.model), chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "[CHIP ] features:%s%s%s%s%s",
             (chip.features & CHIP_FEATURE_WIFI_BGN)  ? " WiFi"     : "",
             (chip.features & CHIP_FEATURE_BLE)       ? " BLE"      : "",
             (chip.features & CHIP_FEATURE_IEEE802154)? " 802.15.4" : "",
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? " emb-flash": "",
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? " emb-psram": "");

    /* ---- ② Flash 容量（N16R8 应为 16MB） ---- */
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[FLASH] size=%lu MB %s",
                 (unsigned long)(flash_size / (1024 * 1024)),
                 (flash_size >= 16u * 1024u * 1024u) ? "(OK: N16R8)" : "(!! 期望 16MB)");
    } else {
        ESP_LOGE(TAG, "[FLASH] get_size failed: %s", esp_err_to_name(err));
    }

    /* ---- ③ PSRAM 容量（Octal 模式应为 8MB） ---- */
    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "[PSRAM] size=%u MB (OK: Octal)",
                 (unsigned)(psram_size / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "[PSRAM] not detected (检查 sdkconfig 的 SPIRAM_MODE_OCT)");
    }

    /* ---- ④ 出厂 MAC ---- */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "[MAC  ] STA %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    /* ---- ⑤ 复位原因（定位启动异常的关键线索） ---- */
    ESP_LOGI(TAG, "[RESET] reason=%s", reset_reason_str(esp_reset_reason()));

    /* ---- ⑥ 堆水位 ---- */
    ESP_LOGI(TAG, "[HEAP ] internal free=%u B, min=%u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "[HEAP ] total free(incl PSRAM)=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "===================================================");
}

/* ================================================================
 *                        心跳任务
 * ================================================================ */

/**
 * @brief  心跳任务：周期性打印运行时长与堆水位
 * @param  arg 未使用
 * @note   纯打印无业务，目的是让"调度器在跑"这件事在日志里可见；
 *         后续接 WiFi/BLE/串口桥时，此任务可保留作系统健康指示。
 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;

    for (;;) {
        beat++;
        ESP_LOGI(TAG, "[BEAT ] #%lu uptime=%lld s | heap free=%u B min=%u B",
                 (unsigned long)beat,
                 esp_timer_get_time() / 1000000,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));   /* 让出 CPU，验证调度 */
    }
}

/* ================================================================
 *                        入口
 * ================================================================ */

/**
 * @brief  应用入口：bringup 自检
 * @note   IDF v5.x 在 core0 上以 app_main 为 FreeRTOS 任务入口；
 *         函数返回后该任务被删除，故此处不返回（保持任务存活亦可）。
 */
void app_main(void)
{
    /* ---- NVS 初始化：后续 WiFi/BLE 凭据与配置的存储底座 ---- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），擦除后重试", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "[NVS  ] init OK");

    /* ---- 系统自检信息 ---- */
    print_system_info();

    /* ---- 板级初始化：所有板载外设一次到位
     * （components/bsp 内部按依赖排序、失败自动回滚、可选模块只警告） ---- */
    err = bsp_init(BSP_MOD_ALL);
    if (err != ESP_OK) {
        /* 不 return：让下面的 bsp_print_info() 把"到底哪个器件没起来"完整打出来 */
        ESP_LOGE(TAG, "[BSP  ] init failed: %s", esp_err_to_name(err));
    }
    bsp_print_info();

    /* ---- 启动心跳任务（4KB 栈，优先级 5 低于主任务） ---- */
    BaseType_t ok = xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "heartbeat task create failed");
        return;
    }

    ESP_LOGI(TAG, "bringup done. 屏幕上的开机动画会逐项自检，无需串口。");

    /* ---- 开机自检动画（屏幕独占，日志照常打印） ---- */
    boot_ui_start();
}
