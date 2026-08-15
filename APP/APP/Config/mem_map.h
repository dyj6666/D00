/* ================================================================
 * mem_map —— 外部 SRAM 布局单一事实源（FSMC NE3，1MB @0x68000000）
 *
 * 架构位置：APP 配置层；任何外部 SRAM 静态地址必须从这里取，
 *           禁止在其他模块写裸地址（曾分散在 la_config/lv_port_disp/
 *           la_sample/lv_conf 四处，扩容互相踩踏）。
 * 编译期重叠断言：改动任一常量，越界/重叠立即 #error 暴露。
 * ================================================================ */
#ifndef MEM_MAP_H
#define MEM_MAP_H

#include <stdint.h>

#define EXT_SRAM_BASE       0x68000000u          /* FSMC NE3 片选基址 */
#define EXT_SRAM_SIZE       (1024u * 1024u)      /* IS62WV51216 1MB */

/* ---------------- 逻辑分析仪采样区（总 512KB） ---------------- */
#define MEM_LA_BASE         0x68000000u
#define MEM_LA_DMA_POINTS   32768u               /* DMA 流：32768 点 × 4B IDR = 128KB */
#define MEM_LA_AREA_SIZE    (512u * 1024u)       /* DMA 流 + 时间戳模式共用 */

/* ---------------- LVGL 堆（lv_conf.h LV_MEM_ADR 引用本地址） ---------------- */
#define MEM_LVGL_HEAP_BASE  0x68080000u
#define MEM_LVGL_HEAP_SIZE  (128u * 1024u)

/* ---------------- LVGL 绘制双缓冲（240×40 像素 × RGB565 × 2 块） ---------------- */
#define MEM_LVGL_DISP_BASE  0x680A0000u
#define MEM_LVGL_DISP_SIZE  (240u * 40u * 2u * 2u)

/* ---------------- LA 预触发环形缓冲（1024 点 × LA_SamplePoint 6B） ---------------- */
#define MEM_LA_PRETRIG_BASE 0x680A9A00u
#define MEM_LA_PRETRIG_SIZE (1024u * 6u)

/* ---------------- 编译期重叠断言（扩容前先核对本表） ---------------- */
#if (MEM_LA_BASE + MEM_LA_AREA_SIZE) > MEM_LVGL_HEAP_BASE
#error "mem_map: LA sampling area overlaps LVGL heap"
#endif
#if (MEM_LVGL_HEAP_BASE + MEM_LVGL_HEAP_SIZE) > MEM_LVGL_DISP_BASE
#error "mem_map: LVGL heap overlaps LVGL disp buffers"
#endif
#if (MEM_LVGL_DISP_BASE + MEM_LVGL_DISP_SIZE) > MEM_LA_PRETRIG_BASE
#error "mem_map: LVGL disp buffers overlap LA pre-trigger buffer"
#endif
#if (MEM_LA_PRETRIG_BASE + MEM_LA_PRETRIG_SIZE) > (EXT_SRAM_BASE + EXT_SRAM_SIZE)
#error "mem_map: LA pre-trigger buffer exceeds external SRAM"
#endif

#endif /* MEM_MAP_H */
