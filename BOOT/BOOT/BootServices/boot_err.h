#ifndef BOOT_ERR_H
#define BOOT_ERR_H

#include <stdint.h>

/* ================================================================
 * BOOT 轻量纠错（崩溃诊断 + 原因复现 + 软复位恢复）
 *
 * 设计原则（bootloader 专用，比 APP 层更精简）：
 *   - 无 RTOS：不需要任务名/堆栈回溯/RTOS 钩子；
 *   - 全 fault 统一入口（汇编捕获现场），打印诊断后软复位，绝不死循环；
 *   - 崩溃摘要持久化到 RTC 备份寄存器 reg 16-19（与 APP err_mgr 的
 *     reg 1-15 隔离，reg 0 保留给 OTA 标志），启动时自动复现原因；
 *   - 不做防抖锁定：BOOT 必须能自动恢复（持续崩溃由升级流程兜底）。
 * ================================================================ */

/* ---------- BKP 摘要布局（reg 16-19，BOOT 专用） ---------- */
#define BOOT_ERR_BKP_MAGIC_REG   16u
#define BOOT_ERR_BKP_SRCSEQ_REG  17u
#define BOOT_ERR_BKP_PC_REG      18u
#define BOOT_ERR_BKP_CRC_REG     19u
#define BOOT_ERR_BKP_MAGIC_VAL   0x42545231u   /* 'BTR1' */

/* ---------- 错误来源（与 APP err_mgr 枚举值对齐，便于统一解读） ---------- */
typedef enum {
    BOOT_ERR_NMI = 1,
    BOOT_ERR_HARDFAULT,
    BOOT_ERR_MEMMANAGE,
    BOOT_ERR_BUSFAULT,
    BOOT_ERR_USAGEFAULT,
} boot_err_src_t;

/* Fault 统一入口：由各 fault handler 的 __asm 入口调用。
 * stack_frame = 异常压栈帧（r0-r3, r12, lr, pc, psr）
 * exc_return  = 现场 EXC_RETURN；src = 错误来源 */
void Boot_ErrFaultEntry(uint32_t *stack_frame, uint32_t exc_return,
                        uint32_t src);

/* 启动时复现上次 BOOT 崩溃（在 USART2/printf 就绪后调用） */
void Boot_ErrReportLast(void);

#endif
