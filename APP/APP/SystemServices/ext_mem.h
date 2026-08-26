/* ================================================================
 * ext_mem —— 外部 SRAM 统一内存池接口
 *
 * 架构位置：APP 服务层；LVGL 内存后端（lv_conf.h LV_MEM_CUSTOM）
 *           与图像缓存/大缓冲的通用分配器。
 *
 * 平台无关：默认 FreeRTOS 互斥量保护；编译宏 EXT_MEM_NO_OS 去除
 *           OS 依赖（主机单元测试用）。
 * ================================================================ */
#ifndef EXT_MEM_H
#define EXT_MEM_H

#include <stdint.h>

/* 池运行统计（GUI 面板/日志直读） */
typedef struct {
    uint32_t total;        /* 池总字节数 */
    uint32_t used;         /* 当前已用字节数 */
    uint32_t peak;         /* 历史峰值已用 */
    uint32_t max_free;     /* 最大连续空闲块（碎片程度） */
    uint32_t alloc_cnt;    /* 分配次数 */
    uint32_t free_cnt;     /* 释放次数 */
    uint32_t realloc_cnt;  /* 重分配次数 */
    uint32_t fail_cnt;     /* 分配失败次数 */
    uint32_t canary_fail;  /* canary 越界检测失败次数（>0 说明有越界写） */
} ext_mem_stats_t;

/* 初始化内存池。base 必须 8 字节对齐；返回 0=成功，非 0=参数非法 */
int    ExtMem_Init(void *base, uint32_t size);

/* 分配 size 字节（8 字节对齐返回）；失败返回 NULL（size=0 也返回 NULL） */
void  *ExtMem_Alloc(uint32_t size);

/* 释放；ptr=NULL 安全；越界写会在释放时被 canary 捕获并计数 */
void   ExtMem_Free(void *ptr);

/* 重分配：扩容搬移并保留旧数据，缩容原地返回（不缩块） */
void  *ExtMem_Realloc(void *ptr, uint32_t size);

/* 快照统计（含最大空闲块扫描） */
void   ExtMem_GetStats(ext_mem_stats_t *out);

/* 全池结构巡检（魔数/边界标记/地址范围）；返回损坏点数，0=完好 */
int    ExtMem_CheckIntegrity(void);

#endif /* EXT_MEM_H */
