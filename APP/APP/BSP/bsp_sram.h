/* ================================================================
 * bsp_sram —— 板载外部 SRAM（IS62WV51216 1MB，FSMC NE3）驱动接口
 *
 * 架构位置：APP BSP 层；内存池（ext_mem）依赖本驱动完成
 *           硬件验证与基准测量。
 *
 * 职责：FSMC BANK3 使能验证 / 上电自检（数据线·地址线·模式）/
 *       吞吐基准（DWT 周期计时）/ 32 位对齐快速拷贝。
 *
 * 时序说明：FSMC 异步 16 位总线（AccessMode A，读写共用 BTR），
 *   ADDSET=2 DATAST=8 → ~59.5ns/16bit 访问，满足 IS62WV51216-55ns
 *   （tRC/tWC=55ns）并留裕量；F407 无 Cache、FSMC 无 DMA 直连，
 *   吞吐上限由总线时序决定——实测数据见启动日志 [SRAM] bench 与
 *   GUI SRAM 页。
 * ================================================================ */
#ifndef BSP_SRAM_H
#define BSP_SRAM_H

#include <stdint.h>

/* 自检/基准结果（GUI 面板与启动日志直读） */
typedef struct {
    uint8_t  fsmc_en;        /* FSMC BANK3 使能标志（1=已使能） */
    uint8_t  test_dat;       /* 数据线走位测试：0=通过，非 0=失败位 */
    uint8_t  test_adr;       /* 地址线走位测试：0=通过 */
    uint8_t  test_pat;       /* 模式翻转/随机测试：0=通过 */
    uint32_t addset;         /* 回读 FSMC BTR3 ADDSET */
    uint32_t datast;         /* 回读 FSMC BTR3 DATAST */
    uint32_t write32_mbps;   /* 32 位写吞吐 MB/s */
    uint32_t read32_mbps;    /* 32 位读吞吐 MB/s */
    uint32_t write16_mbps;   /* 16 位写吞吐 MB/s */
    uint32_t read16_mbps;    /* 16 位读吞吐 MB/s */
    uint32_t copy_mbps;      /* 32 位对齐拷贝吞吐 MB/s */
} bsp_sram_status_t;

/* 模块初始化：FSMC 验证 → 自检 → 基准 → ExtMem_Init（模块注册表调用，
 * priority=2，早于 LVGL/GuiApp；失败仅记录，不阻塞启动） */
void BSP_SRAM_Init(void);

/* 获取最近一次自检/基准结果快照 */
const bsp_sram_status_t *BSP_SRAM_GetStatus(void);

/* 32 位对齐快速拷贝（FSMC 16 位总线：word 访问 = 2 个背靠背 16 位
 * 总线周期，指令开销减半；非对齐输入自动回退字节拷贝） */
void BSP_SRAM_Memcpy32(void *dst, const void *src, uint32_t bytes);

#endif /* BSP_SRAM_H */
