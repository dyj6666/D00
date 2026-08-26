/* ================================================================
 * mem_map —— 外部 SRAM 布局单一事实源（FSMC NE3，1MB @0x68000000）
 *
 * 架构位置：APP 配置层；任何外部 SRAM 静态地址必须从这里取，
 *           禁止在其他模块写裸地址。
 * 编译期重叠断言：改动任一常量，越界/重叠立即 #error 暴露。
 *
 * 布局（2026-08 重构：统一内存池替代分散固定槽）：
 *   0x68000000  LA 采样区 512KB（DMA 流 + 时间戳模式共用，稳定不动）
 *   0x680A0000  LA 预触发环形缓冲 6KB（1024 点 × 6B；自旧址 0x680A9A00
 *               迁入已废弃的 LVGL 绘制缓冲槽，为统一池让位）
 *   0x680A2000  ExtMem 统一内存池 376KB（LVGL 对象池/图像缓存/大缓冲共用，
 *               ext_mem.c 统一管理；替代旧 LV_MEM_ADR 128KB 静态池）
 *   0x68100000  1MB 顶
 * ================================================================ */
#ifndef MEM_MAP_H
#define MEM_MAP_H

#include <stdint.h>

#define EXT_SRAM_BASE       0x68000000u          /* FSMC NE3 片选基址 */
#define EXT_SRAM_SIZE       (1024u * 1024u)      /* IS62WV51216 1MB */

/* ---------------- 逻辑分析仪采样区（总 512KB，DMA 流 + 时间戳模式共用） ---------------- */
#define MEM_LA_BASE         0x68000000u
#define MEM_LA_AREA_SIZE    (512u * 1024u)

/* ---------------- LA 预触发环形缓冲（1024 点 × LA_SamplePoint 6B） ---------------- */
#define MEM_LA_PRETRIG_BASE 0x680A0000u
#define MEM_LA_PRETRIG_SIZE (1024u * 6u)

/* ---------------- ExtMem 统一内存池（LVGL + 图像缓存 + 大缓冲） ---------------- */
#define MEM_EXT_POOL_BASE   0x680A2000u
#define MEM_EXT_POOL_SIZE   (0x68100000u - MEM_EXT_POOL_BASE)   /* 376KB 到 1MB 顶 */

/* ---------------- 编译期重叠断言（扩容前先核对本表） ---------------- */
#if (MEM_LA_BASE + MEM_LA_AREA_SIZE) > MEM_LA_PRETRIG_BASE
#error "mem_map: LA sampling area overlaps LA pre-trigger buffer"
#endif
#if (MEM_LA_PRETRIG_BASE + MEM_LA_PRETRIG_SIZE) > MEM_EXT_POOL_BASE
#error "mem_map: LA pre-trigger buffer overlaps ExtMem pool"
#endif
#if (MEM_EXT_POOL_BASE + MEM_EXT_POOL_SIZE) > (EXT_SRAM_BASE + EXT_SRAM_SIZE)
#error "mem_map: ExtMem pool exceeds external SRAM"
#endif

#endif /* MEM_MAP_H */
