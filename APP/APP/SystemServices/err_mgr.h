/* ================================================================
 * err_mgr —— 错误管理：错误码登记与查询
 *
 * 架构位置：APP 服务层；崩溃/复位原因记录
 * ================================================================ */
#ifndef ERR_MGR_H
#define ERR_MGR_H

#include <stdint.h>

/* ================================================================
 * 系统错误管理（崩溃诊断 + 原因复现 + 安全恢复）
 *
 * 覆盖：
 *   - Cortex-M4 全部 fault（NMI/HardFault/MemManage/BusFault/UsageFault）
 *   - 未处理中断（Default_Handler）
 *   - RTOS 层：configASSERT / 栈溢出 / 任务看门狗 stall
 *
 * 机制：
 *   1) 崩溃现场完整采集（寄存器/栈帧/fault 状态寄存器/当前任务/堆栈回溯）
 *   2) 完整报告经调试串口输出（裸寄存器轮询，不依赖可能已损坏的 RTOS）
 *   3) 摘要持久化到 RTC 备份寄存器（BKP），复位后启动即可复现崩溃原因
 *   4) 默认软复位恢复（非死循环）；连续快速崩溃触发防抖锁定，防复位风暴
 * ================================================================ */

/* ---------- 错误来源 ---------- */
typedef enum {
    ERR_SRC_NMI = 1,          /* 不可屏蔽中断（时钟安全/电源等） */
    ERR_SRC_HARDFAULT,        /* HardFault（含 FORCED 级联） */
    ERR_SRC_MEMMANAGE,        /* MemManage（MPU 违规） */
    ERR_SRC_BUSFAULT,         /* BusFault（总线访问错误） */
    ERR_SRC_USAGEFAULT,       /* UsageFault（未定义指令/非法状态/除零） */
    ERR_SRC_RTOS_ASSERT,      /* FreeRTOS configASSERT 失败 */
    ERR_SRC_STACK_OVERFLOW,   /* FreeRTOS 栈溢出检测 */
    ERR_SRC_TASK_STALL,       /* 任务看门狗超时 */
    ERR_SRC_UNHANDLED_IRQ,    /* 未处理中断进入 Default_Handler */
} err_src_t;

/* ---------- 崩溃记录（RAM 常驻，供调试器/上位机读取） ---------- */
#define ERR_RECORD_MAGIC      0x45525243u   /* 'ERRC' */
#define ERR_CALL_STACK_MAX    12
#define ERR_TASK_NAME_MAX     16

typedef struct {
    uint32_t magic;            /* ERR_RECORD_MAGIC */
    uint32_t src;              /* err_src_t */
    uint32_t seq;              /* 崩溃序号（BKP 持久递增） */
    uint32_t tick_ms;          /* 崩溃时系统运行时长 */
    uint32_t pc;               /* 崩溃指令地址 */
    uint32_t lr;               /* 返回地址 */
    uint32_t psr;              /* 程序状态字 */
    uint32_t cfsr;             /* 可配置 fault 状态寄存器 */
    uint32_t hfsr;             /* 硬 fault 状态寄存器 */
    uint32_t fault_addr;       /* BFAR / MMFAR */
    uint32_t exc_return;       /* EXC_RETURN（线程/处理模式判别） */
    uint32_t r0, r1, r2, r3, r12;
    uint32_t msp, psp;
    char     task_name[ERR_TASK_NAME_MAX];  /* 崩溃时当前任务（线程模式） */
    char     cause[56];        /* 人类可读故障原因 */
    uint32_t call_stack[ERR_CALL_STACK_MAX]; /* 堆栈回溯（PC 列表） */
    uint32_t call_depth;
    uint32_t crc;              /* 记录完整性校验 */
} err_record_t;

/* ---------- 接口 ---------- */

/* 初始化：清 RAM 记录。应在 RTOS 启动后、sysmon 之前调用。 */
void ERR_Init(void);

/* 启动时复现上次崩溃（读 BKP 摘要打印到调试串口）。 */
void ERR_ReportLastCrash(void);

/* Fault 统一入口。由各 fault handler 的 naked 汇编包装调用：
 *   stack_frame = 异常压栈帧（r0-r3, r12, lr, pc, psr）
 *   exc_return  = 现场 LR（EXC_RETURN，用于线程/处理模式判别）
 *   src         = 错误来源 */
void ERR_HandleFaultEntry(uint32_t *stack_frame, uint32_t exc_return,
                          uint32_t src);

/* FreeRTOS configASSERT 失败入口。 */
void ERR_HandleAssert(uint32_t line);

/* FreeRTOS 栈溢出检测入口。 */
void ERR_HandleStackOverflow(const char *task_name);

/* 任务看门狗 stall 入口。 */
void ERR_HandleTaskStall(const char *task_name, uint32_t silent_ms);

/* 未处理中断入口（Default_Handler 调用，irqn 为当前向量号）。 */
void ERR_HandleUnhandledIRQ(uint32_t irqn);

/* 查询当前崩溃序号（用于监控/上报）。 */
uint32_t ERR_GetCrashSeq(void);

/* 系统运行时长快照（ms）：由 SysTick 钩子每 tick 更新。
 * 供 err_mgr 在任意优先级中断/异常中读取，避免 FreeRTOS ISR API
 * 的优先级断言（portASSERT_IF_INTERRUPT_PRIORITY_INVALID）。 */
extern volatile uint32_t ERR_TickMs;

/* 获取最近一次崩溃记录（RAM）。 */
const err_record_t *ERR_GetLastRecord(void);

/* 打印一条崩溃记录的完整报告（调试串口）。 */
void ERR_DumpRecord(const err_record_t *rec);

#endif
