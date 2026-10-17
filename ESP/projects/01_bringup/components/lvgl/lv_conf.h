/**
 * @file lv_conf.h
 * LVGL 配置 —— ESP32-S3-BOX（AI BOX1）/ 320x240 ST7789 8080 并口
 *
 * 调优思路（性能目标：稳定 60fps、输入零延迟）：
 *   1. LV_COLOR_DEPTH=16：与面板一致，不做转换；
 *   2. 渲染内存池放内部 RAM（比 PSRAM 快一个量级），64KB 足够仪表盘类 UI；
 *   3. 刷新周期 15ms、输入 20ms：与 8080 总线带宽（10MB/s → 全屏 15.4ms）匹配；
 *   4. LV_TICK_CUSTOM 用 esp_timer_get_time()，省掉一个 1ms 定时器中断；
 *   5. 只启用实际用到的控件与字体，减少 flash 占用与初始化开销。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
/* IRAM_ATTR：把 LVGL 的热路径（混合/字形/填充）放进内部 IRAM，
 * 避免 flash XIP 取指 cache miss——实测可显著缩短整帧渲染时间。 */
#include <esp_attr.h>

/*==================== 颜色 ====================*/
/* RGB565；面板 IO 已配置 swap_color_bytes，缓冲内放标准 RGB565 即可 */
#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP        0
#define LV_COLOR_SCREEN_TRANSP  0

/*==================== 内存 ====================*/
/* 静态池放在内部 SRAM（.bss）——LVGL 对象/样式/绘制临时区，速度优先 */
#define LV_MEM_CUSTOM           0
#define LV_MEM_SIZE             (64U * 1024U)
#define LV_MEM_ADR              0
#define LV_MEM_BUF_MAX_NUM      16
#define LV_MEMCPY_MEMSET_STD    1

/*==================== HAL ====================*/
#define LV_DISP_DEF_REFR_PERIOD  15     /* 刷新定时周期(ms)：66Hz 上限 */
#define LV_INDEV_DEF_READ_PERIOD 20     /* 输入扫描周期(ms)：触摸响应 ≤20ms */
#define LV_DPI_DEF               130

/* 系统时基直接取 esp_timer（微秒）——无需 lv_tick_inc 中断 */
#define LV_TICK_CUSTOM           1
#define LV_TICK_CUSTOM_INCLUDE   <esp_timer.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000))

/*==================== 绘制 ====================*/
#define LV_DRAW_COMPLEX          1      /* 阴影/渐变/圆角需要 */
#define LV_SHADOW_CACHE_SIZE     0
#define LV_CIRCLE_CACHE_SIZE     8
#define LV_IMG_CACHE_DEF_SIZE    0
#define LV_GRADIENT_MAX_STOPS    2

/*==================== 日志 ====================*/
#define LV_USE_LOG               1
#define LV_LOG_LEVEL             LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF            0      /* 由 ui_port.c 注册回调转发到 ESP_LOG */
#define LV_LOG_USE_TIMESTAMP     1
#define LV_LOG_USE_FILE_LINE     1

/*==================== 断言 ====================*/
#define LV_USE_ASSERT_NULL           1
#define LV_USE_ASSERT_MALLOC         1
#define LV_USE_ASSERT_STYLE          0
#define LV_USE_ASSERT_MEM_INTEGRITY  0
#define LV_USE_ASSERT_OBJ            0

/*==================== 性能监视 ====================*/
/* FPS/CPU 叠加显示：右上角小字，界面验收时开，量产可关 */
#define LV_USE_PERF_MONITOR      1
#define LV_USE_MEM_MONITOR       0
#define LV_USE_REFR_DEBUG        0

/*==================== 字体（按需开启，控制 flash）====================*/
#define LV_FONT_MONTSERRAT_12    1
#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_16    1
#define LV_FONT_MONTSERRAT_20    1
#define LV_FONT_MONTSERRAT_24    0
#define LV_FONT_MONTSERRAT_28    1
#define LV_FONT_MONTSERRAT_32    0
#define LV_FONT_MONTSERRAT_40    1
#define LV_FONT_MONTSERRAT_48    0
#define LV_FONT_DEFAULT          &lv_font_montserrat_16

#define LV_FONT_FMT_TXT_LARGE    0
#define LV_USE_FONT_COMPRESSED   0
#define LV_USE_FONT_SUBPX        0

/*==================== 控件 ====================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      1
#define LV_USE_CHART      1
#define LV_USE_METER      1
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_SPINBOX    0
#define LV_USE_SPAN       0
#define LV_USE_ANIMIMG    1
#define LV_USE_CALENDAR   0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINNER    1

/*==================== 布局与主题 ====================*/
#define LV_USE_FLEX              1
#define LV_USE_GRID              1
#define LV_USE_THEME_DEFAULT     1
#define LV_THEME_DEFAULT_DARK    1
#define LV_THEME_DEFAULT_GROW    1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC       0
#define LV_USE_THEME_MONO        0

/*==================== 第三方库（全部关闭，省 flash）====================*/
#define LV_USE_FS_STDIO   0
#define LV_USE_FS_POSIX   0
#define LV_USE_FS_WIN32   0
#define LV_USE_FS_FATFS   0
#define LV_USE_PNG        0
#define LV_USE_BMP        0
#define LV_USE_SJPG       0
#define LV_USE_GIF        0
#define LV_USE_QRCODE     0
#define LV_USE_FREETYPE   0
#define LV_USE_RLOTTIE    0
#define LV_USE_FFMPEG     0

/*==================== 杂项 ====================*/
#define LV_USE_SNAPSHOT   0
#define LV_USE_MONKEY     0
#define LV_USE_GRIDNAV    0
#define LV_USE_FRAGMENT   0
#define LV_USE_IMGFONT    0
#define LV_USE_MSG        0
#define LV_USE_IME_PINYIN 0
#define LV_USE_USER_DATA  1

/*==================== 性能：热路径放 IRAM ====================*/
/* LVGL 用该宏标注混合/字形/填充等热点函数；ESP32 上映射到 IRAM_ATTR，
 * 让渲染不依赖 flash 指令 cache（内部 RAM 有富余时强烈建议开）。 */
#define LV_ATTRIBUTE_FAST_MEM  IRAM_ATTR

#endif /* LV_CONF_H */
