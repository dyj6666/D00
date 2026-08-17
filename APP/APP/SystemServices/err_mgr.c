/* ================================================================
 * err_mgr —— 错误管理：错误码记录与崩溃原因查询
 *
 * 架构位置：APP 服务层；复位原因诊断
 * ================================================================ */
#include "err_mgr.h"
#if defined(__GNUC__)
#define ERR_RETURN_ADDR()   __builtin_return_address(0)
#else
#define ERR_RETURN_ADDR()   __return_address()
#endif
#include "bsp.h"
#include "app_config.h"
#include "usart.h"
#include "rtc.h"
#include "logger.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdio.h>

/* FreeRTOS 内部 TCB 指针（tasks.c 定义，task.h 未导出）：
 * 中断/异常上下文直接读取当前任务，避免 xTaskGetCurrentTaskHandle()
 * 内部的 taskENTER_CRITICAL（ISR 中非法）。*/
struct tskTaskControlBlock;
extern struct tskTaskControlBlock * volatile pxCurrentTCB;

/* ================================================================
 * 系统错误管理实现
 * ================================================================ */

/* ---------- BKP 摘要布局（reg 0 保留给 OTA BOOT_FLAG_UPGRADE；---------- */
#define ERR_BKP_MAGIC_REG    1u
#define ERR_BKP_SRC_REG      2u
#define ERR_BKP_SEQ_REG      3u
#define ERR_BKP_PC_REG       4u
#define ERR_BKP_LR_REG       5u
#define ERR_BKP_ADDR_REG     6u
#define ERR_BKP_CFSR_REG     7u
#define ERR_BKP_HFSR_REG     8u
#define ERR_BKP_TICK_REG     9u
#define ERR_BKP_NAME_REG0    10u   /* task_name[0..3] */
#define ERR_BKP_NAME_REG1    11u   /* task_name[4..7] */
#define ERR_BKP_NAME_REG2    12u   /* task_name[8..11] */
#define ERR_BKP_CRC_REG      13u   /* 摘要完整性校验*/
#define ERR_BKP_RAPID_REG    14u   /* 快速崩溃计数*/
#define ERR_BKP_RTC_REG      15u   /* 上次崩溃 RTC 秒数 */
#define ERR_BKP_MAGIC_VAL    0x45525231u  /* 'ERR1' */

#define ERR_RAPID_WINDOW_S   10u   /* 快速崩溃判定窗口（秒） */
#define ERR_RAPID_LIMIT      3u    /* 连续快速崩溃锁定阈值*/
#define ERR_BACKTRACE_SCAN   512u  /* 栈回溯扫描深度（字节）*/
#define ERR_FLASH_CODE_BASE  0x08000000u
#define ERR_FLASH_CODE_END   0x08100000u
#define ERR_SRAM_BASE        0x20000000u
#define ERR_SRAM_END         0x20020000u

static err_record_t g_rec;
static volatile uint8_t g_locked = 0;
volatile uint32_t ERR_TickMs = 0;

/* 崩溃序号：BKP 摘要有效则递增，否则从头计数（防止残留垃圾数据）*/
static uint32_t err_next_seq(void)
{
    uint32_t magic = BSP_RTC_ReadBackupReg(ERR_BKP_MAGIC_REG);
    uint32_t seq = BSP_RTC_ReadBackupReg(ERR_BKP_SEQ_REG);
    if ((magic & 0xFF) != (ERR_BKP_MAGIC_VAL & 0xFF)) {
        seq = 0;
    }
    return seq + 1u;
}

/* 记录当前任务名（线程模式有效；处理模式标 ISR）*/
static void err_capture_task(char *out, uint32_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    /* 直接读 pxCurrentTCB：xTaskGetCurrentTaskHandle() 内部使用
     * taskENTER_CRITICAL，在中断/异常上下文非法（可导致死锁）。*/
    TaskHandle_t h = pxCurrentTCB;
    if (h != NULL) {
        const char *name = pcTaskGetName(h);
        if (name != NULL && name[0] != '\0') {
            strncpy(out, name, cap - 1);
            out[cap - 1] = '\0';
            return;
        }
    }
    strncpy(out, "(ISR)", cap - 1);
    out[cap - 1] = '\0';
}

/* ---------- 来源名称 ---------- */
static const char *err_src_name(err_src_t src)
{
    switch (src) {
    case ERR_SRC_NMI:            return "NMI";
    case ERR_SRC_HARDFAULT:      return "HardFault";
    case ERR_SRC_MEMMANAGE:      return "MemManage";
    case ERR_SRC_BUSFAULT:       return "BusFault";
    case ERR_SRC_USAGEFAULT:     return "UsageFault";
    case ERR_SRC_RTOS_ASSERT:    return "RTOS Assert";
    case ERR_SRC_STACK_OVERFLOW: return "Stack Overflow";
    case ERR_SRC_TASK_STALL:     return "Task Stall";
    case ERR_SRC_UNHANDLED_IRQ:  return "Unhandled IRQ";
    default:                     return "Unknown";
    }
}

/* ---------- 裸寄存器 UART 输出（不依赖 RTOS/HAL 状态） ---------- */
static void err_uart_putc(uint8_t c)
{
    while (!(huart3.Instance->SR & USART_SR_TXE)) {
    }
    huart3.Instance->DR = c;
}

static void err_uart_puts(const char *s)
{
    while (*s) {
        err_uart_putc((uint8_t)*s++);
    }
}

static void err_uart_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    err_uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        err_uart_putc((uint8_t)hex[(v >> i) & 0xF]);
    }
}

static void err_uart_dec32(uint32_t v)
{
    char buf[12];
    int n = 0;
    if (v == 0) {
        err_uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        err_uart_putc((uint8_t)buf[--n]);
    }
}

/* ---------- fault 状态解码---------- */
static void err_decode_cause(err_src_t src, uint32_t cfsr, uint32_t hfsr,
                             char *out, uint32_t cap)
{
    const char *s = "";
    if (cfsr & (1u << 25)) s = "DIVBYZERO";
    else if (cfsr & (1u << 24)) s = "UNALIGNED";
    else if (cfsr & (1u << 19)) s = "NOCP (coprocessor)";
    else if (cfsr & (1u << 18)) s = "INVPC (invalid PC load)";
    else if (cfsr & (1u << 17)) s = "INVSTATE (invalid state)";
    else if (cfsr & (1u << 16)) s = "UNDEFINSTR";
    else if (cfsr & (1u << 15)) s = "BFARVALID";
    else if (cfsr & (1u << 13)) s = "LSPERR";
    else if (cfsr & (1u << 12)) s = "STKERR (stacking)";
    else if (cfsr & (1u << 11)) s = "UNSTKERR (unstacking)";
    else if (cfsr & (1u << 10)) s = "IMPRECISERR";
    else if (cfsr & (1u << 9))  s = "PRECISERR";
    else if (cfsr & (1u << 8))  s = "IBUSERR";
    else if (cfsr & (1u << 7))  s = "MMFARVALID";
    else if (cfsr & (1u << 5))  s = "MLSPERR";
    else if (cfsr & (1u << 4))  s = "MSTKERR";
    else if (cfsr & (1u << 3))  s = "MUNSTKERR";
    else if (cfsr & (1u << 1))  s = "DACCVIOL";
    else if (cfsr & (1u << 0))  s = "IACCVIOL";
    else if (hfsr & (1u << 30)) s = "FORCED (nested)";
    else s = "UNKNOWN";

    snprintf(out, cap, "%s: %s", err_src_name(src), s);
}

/* ---------- 代码地址判定（Thumb 位 1，Flash 范围内） ---------- */
static int err_is_code(uint32_t v)
{
    return (v >= ERR_FLASH_CODE_BASE && v < ERR_FLASH_CODE_END && (v & 1u));
}

/* ---------- 堆栈回溯（启发式扫描，返回调用链 PC 列表）---------- */
static uint32_t err_backtrace(uint32_t sp, uint32_t lr, uint32_t pc,
                              uint32_t *out, uint32_t max)
{
    uint32_t n = 0;

    if (err_is_code(pc) && n < max) out[n++] = pc;
    if (err_is_code(lr) && n < max) out[n++] = lr;

    /* 栈指针必须落在 SRAM 范围内才扫描，避免二次 fault */
    if (sp < ERR_SRAM_BASE || sp >= ERR_SRAM_END) {
        return n;
    }
    if (sp + ERR_BACKTRACE_SCAN > ERR_SRAM_END) {
        return n;
    }

    const uint32_t *p = (const uint32_t *)(sp & ~3u);
    for (uint32_t i = 0; i < ERR_BACKTRACE_SCAN / 4u && n < max; i++) {
        uint32_t v = p[i];
        if (!err_is_code(v)) continue;
        int dup = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (out[j] == v) {
                dup = 1;
                break;
            }
        }
        if (!dup) out[n++] = v;
    }
    return n;
}

/* ---------- BKP 摘要持久化---------- */
static uint32_t err_bkp_crc(uint32_t *regs, uint32_t n)
{
    uint32_t sum = 0x5A5A5A5Au;
    for (uint32_t i = 0; i < n; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum ^= regs[i];
    }
    return sum;
}

static void err_bkp_save(const err_record_t *rec)
{
    uint32_t regs[12];
    regs[0] = ERR_BKP_MAGIC_VAL;
    regs[1] = rec->src;
    regs[2] = rec->seq;
    regs[3] = rec->pc;
    regs[4] = rec->lr;
    regs[5] = rec->fault_addr;
    regs[6] = rec->cfsr;
    regs[7] = rec->hfsr;
    regs[8] = rec->tick_ms;
    memcpy(&regs[9], rec->task_name, 12);          /* regs[9..11] */
    uint32_t crc = err_bkp_crc(regs, 12);          /* 摘要 CRC（12 项） */

    BSP_RTC_WriteBackupReg(ERR_BKP_MAGIC_REG, regs[0]);
    BSP_RTC_WriteBackupReg(ERR_BKP_SRC_REG, regs[1]);
    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, regs[2]);
    BSP_RTC_WriteBackupReg(ERR_BKP_PC_REG, regs[3]);
    BSP_RTC_WriteBackupReg(ERR_BKP_LR_REG, regs[4]);
    BSP_RTC_WriteBackupReg(ERR_BKP_ADDR_REG, regs[5]);
    BSP_RTC_WriteBackupReg(ERR_BKP_CFSR_REG, regs[6]);
    BSP_RTC_WriteBackupReg(ERR_BKP_HFSR_REG, regs[7]);
    BSP_RTC_WriteBackupReg(ERR_BKP_TICK_REG, regs[8]);
    BSP_RTC_WriteBackupReg(ERR_BKP_NAME_REG0, regs[9]);
    BSP_RTC_WriteBackupReg(ERR_BKP_NAME_REG1, regs[10]);
    BSP_RTC_WriteBackupReg(ERR_BKP_NAME_REG2, regs[11]);
    BSP_RTC_WriteBackupReg(ERR_BKP_CRC_REG, crc);
}

/* ---------- 快速崩溃防抖（RTC 秒级窗口）---------- */
static uint32_t err_rtc_sec(void)
{
    RTC_TimeTypeDef t;
    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return 0;
    }
    return (uint32_t)t.Hours * 3600u + (uint32_t)t.Minutes * 60u + t.Seconds;
}

static int err_rapid_crash_check(void)
{
    uint32_t now = err_rtc_sec();
    uint32_t last = BSP_RTC_ReadBackupReg(ERR_BKP_RTC_REG);
    uint32_t rapid = BSP_RTC_ReadBackupReg(ERR_BKP_RAPID_REG);

    if (now == 0 || last == 0) {           /* RTC 无效：不防抖 */
        BSP_RTC_WriteBackupReg(ERR_BKP_RTC_REG, now);
        BSP_RTC_WriteBackupReg(ERR_BKP_RAPID_REG, 1);
        return 0;
    }
    if ((now >= last && now - last < ERR_RAPID_WINDOW_S) ||
        (now < last && now + 86400u - last < ERR_RAPID_WINDOW_S)) {
        rapid++;
    } else {
        rapid = 1;
    }
    BSP_RTC_WriteBackupReg(ERR_BKP_RTC_REG, now);
    BSP_RTC_WriteBackupReg(ERR_BKP_RAPID_REG, rapid);
    return (rapid >= ERR_RAPID_LIMIT);
}

/* ---------- 恢复策略：软复位 / 防抖锁定 ---------- */
static void err_delay_loop(uint32_t ms)
{
    /* 中断已禁用，不能使用 HAL_Delay（依赖 SysTick 中断）；裸循环近似*/
    volatile uint32_t n = ms * 16800u;   /* ~168MHz，粗粒度 */
    while (n--) {
    }
}

static void err_recover(void)
{
    if (g_locked) {
        err_uart_puts("\r\n[ERR] FATAL: repeated crashes, system halted (power-cycle to recover).\r\n");
        for (;;) {
            BSP_Watchdog_Refresh();   /* 保持喂狗：锁定等待人工干预，而非复位风暴 */
            BSP_LED_Toggle(1);
            err_delay_loop(500);
        }
    }

#if APP_DEBUG_MODE
    /* 调试模式：崩溃后保持死循环保留现场（BKP 摘要已写，诊断已打印），
     * 供 DAP 取证——绝不软复位销毁现场；人工断电/复位恢复。 */
    err_uart_puts("\r\n[ERR] DEBUG MODE: crash scene preserved, power-cycle to recover.\r\n");
    for (;;) {
        BSP_LED_Toggle(1);
        err_delay_loop(300);
    }
#else
    /* LED 快闪 3 次提示后软复位（复位原因由 BSP_RESET 保留供 sysmon 分析；*/
    for (int i = 0; i < 3; i++) {
        BSP_LED_Toggle(1);
        err_delay_loop(80);
    }
    BSP_SystemReset();
    for (;;) {
    }
#endif
}

/* ---------- 完整转储 ---------- */
void ERR_DumpRecord(const err_record_t *rec)
{
    if (rec == NULL) return;
    /* 直接中止 USART3 TX DMA（不依赖 HAL：异常中断上下文中
     * HAL_UART_AbortTransmit 的锁/等待可能阻塞，导致诊断卡死） */
    if (huart3.hdmatx != NULL) {
        DMA_Stream_TypeDef *tx = huart3.hdmatx->Instance;
        tx->CR &= ~DMA_SxCR_EN;        /* 禁用 TX DMA 流*/
        DMA_TypeDef *dma = (huart3.hdmatx->StreamIndex < 8) ? DMA1 : DMA2;
        dma->LIFCR = 0xFFFFFFFFu;      /* 清低流标志（诊断场景）*/
        dma->HIFCR = 0xFFFFFFFFu;      /* 清高流标志*/
    }
    CLEAR_BIT(huart3.Instance->CR3, USART_CR3_DMAT);

    char buf[96];
    err_uart_puts("\r\n");
    err_uart_puts("===========================================================\r\n");
    err_uart_puts("          SYSTEM FAULT DIAGNOSTIC REPORT\r\n");
    err_uart_puts("===========================================================\r\n");
    snprintf(buf, sizeof(buf), "Source   : %s\r\n", err_src_name((err_src_t)rec->src));
    err_uart_puts(buf);
    snprintf(buf, sizeof(buf), "Sequence : #%lu\r\n", (unsigned long)rec->seq);
    err_uart_puts(buf);
    snprintf(buf, sizeof(buf), "Uptime   : %lu ms\r\n", (unsigned long)rec->tick_ms);
    err_uart_puts(buf);
    snprintf(buf, sizeof(buf), "Cause    : %s\r\n", rec->cause);
    err_uart_puts(buf);
    snprintf(buf, sizeof(buf), "Task     : %s\r\n", rec->task_name[0] ? rec->task_name : "(ISR context)");
    err_uart_puts(buf);

    err_uart_puts("PC       : "); err_uart_hex32(rec->pc); err_uart_puts("\r\n");
    err_uart_puts("LR       : "); err_uart_hex32(rec->lr); err_uart_puts("\r\n");
    err_uart_puts("xPSR     : "); err_uart_hex32(rec->psr); err_uart_puts("\r\n");
    err_uart_puts("EXC_RET  : "); err_uart_hex32(rec->exc_return); err_uart_puts("\r\n");
    err_uart_puts("CFSR     : "); err_uart_hex32(rec->cfsr); err_uart_puts("\r\n");
    err_uart_puts("HFSR     : "); err_uart_hex32(rec->hfsr); err_uart_puts("\r\n");
    err_uart_puts("FAULT_ADDR: "); err_uart_hex32(rec->fault_addr); err_uart_puts("\r\n");

    err_uart_puts("R0       : "); err_uart_hex32(rec->r0);
    err_uart_puts("  R1   : "); err_uart_hex32(rec->r1);
    err_uart_puts("  R2   : "); err_uart_hex32(rec->r2);
    err_uart_puts("  R3   : "); err_uart_hex32(rec->r3); err_uart_puts("\r\n");
    err_uart_puts("R12      : "); err_uart_hex32(rec->r12);
    err_uart_puts("  MSP  : "); err_uart_hex32(rec->msp);
    err_uart_puts("  PSP  : "); err_uart_hex32(rec->psp); err_uart_puts("\r\n");

    err_uart_puts("Call stack (PC list):\r\n");
    for (uint32_t i = 0; i < rec->call_depth; i++) {
        err_uart_puts("  ["); err_uart_dec32(i); err_uart_puts("] ");
        err_uart_hex32(rec->call_stack[i]); err_uart_puts("\r\n");
    }
    err_uart_puts("===========================================================\r\n");
}

/* ---------- 统一 fault 入口 ---------- */
void ERR_HandleFaultEntry(uint32_t *stack_frame, uint32_t exc_return,
                          uint32_t src)
{
    __disable_irq();   /* fault 处理期间冻结系统，防二次干扰 */

    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.magic = ERR_RECORD_MAGIC;
    g_rec.src = src;
    g_rec.seq = err_next_seq();
    g_rec.tick_ms = ERR_TickMs;
    g_rec.cfsr = SCB->CFSR;
    g_rec.hfsr = SCB->HFSR;
    g_rec.exc_return = exc_return;

    if (g_rec.cfsr & (1u << 7)) {
        g_rec.fault_addr = SCB->MMFAR;
    } else if (g_rec.cfsr & (1u << 15)) {
        g_rec.fault_addr = SCB->BFAR;
    }

    if (stack_frame != NULL) {
        g_rec.r0 = stack_frame[0];
        g_rec.r1 = stack_frame[1];
        g_rec.r2 = stack_frame[2];
        g_rec.r3 = stack_frame[3];
        g_rec.r12 = stack_frame[4];
        g_rec.lr = stack_frame[5];
        g_rec.pc = stack_frame[6];
        g_rec.psr = stack_frame[7];
    } else {
        /* NMI 等：MSP 上的标准异常压栈（r0-r3,r12,lr,pc,psr）*/
        const uint32_t *sp = (const uint32_t *)__get_MSP();
        g_rec.r0 = sp[0];
        g_rec.r1 = sp[1];
        g_rec.r2 = sp[2];
        g_rec.r3 = sp[3];
        g_rec.r12 = sp[4];
        g_rec.lr = sp[5];
        g_rec.pc = sp[6];
        g_rec.psr = sp[7];
        if (!err_is_code(g_rec.pc)) g_rec.pc = g_rec.lr;
    }

    /* 线程模式（任务上下文）时记录当前任务名*/
    if (exc_return & 0x04) {
        err_capture_task(g_rec.task_name, sizeof(g_rec.task_name));
    } else {
        strncpy(g_rec.task_name, "(ISR)", ERR_TASK_NAME_MAX - 1);
        g_rec.task_name[ERR_TASK_NAME_MAX - 1] = '\0';
    }

    g_rec.msp = (uint32_t)__get_MSP();
    g_rec.psp = (uint32_t)__get_PSP();
    err_decode_cause((err_src_t)src, g_rec.cfsr, g_rec.hfsr,
                     g_rec.cause, sizeof(g_rec.cause));

    uint32_t sp = (exc_return & 0x04) ? g_rec.psp : g_rec.msp;
    g_rec.call_depth = err_backtrace(sp, g_rec.lr, g_rec.pc,
                                     g_rec.call_stack, ERR_CALL_STACK_MAX);
    g_rec.crc = err_bkp_crc((uint32_t *)&g_rec, sizeof(g_rec) / 4u);

    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, g_rec.seq);
    err_bkp_save(&g_rec);

    ERR_DumpRecord(&g_rec);

    if (err_rapid_crash_check()) {
        g_locked = 1;
    }
    err_recover();
}

/* ---------- RTOS 钩子入口 ---------- */
void ERR_HandleAssert(uint32_t line)
{
    __disable_irq();
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.magic = ERR_RECORD_MAGIC;
    g_rec.src = ERR_SRC_RTOS_ASSERT;
    g_rec.seq = err_next_seq();
    g_rec.tick_ms = ERR_TickMs;
    g_rec.lr = (uint32_t)ERR_RETURN_ADDR();
    g_rec.pc = g_rec.lr;
    err_capture_task(g_rec.task_name, sizeof(g_rec.task_name));
    g_rec.msp = (uint32_t)__get_MSP();
    g_rec.psp = (uint32_t)__get_PSP();
    snprintf(g_rec.cause, sizeof(g_rec.cause),
             "FreeRTOS assert failed at line %lu", (unsigned long)line);
    g_rec.call_depth = err_backtrace(g_rec.msp, g_rec.lr, g_rec.pc,
                                     g_rec.call_stack, ERR_CALL_STACK_MAX);
    g_rec.crc = err_bkp_crc((uint32_t *)&g_rec, sizeof(g_rec) / 4u);

    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, g_rec.seq);
    err_bkp_save(&g_rec);
    ERR_DumpRecord(&g_rec);
    err_recover();
}

void ERR_HandleStackOverflow(const char *task_name)
{
    __disable_irq();
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.magic = ERR_RECORD_MAGIC;
    g_rec.src = ERR_SRC_STACK_OVERFLOW;
    g_rec.seq = err_next_seq();
    g_rec.tick_ms = ERR_TickMs;
    if (task_name != NULL) {
        strncpy(g_rec.task_name, task_name, ERR_TASK_NAME_MAX - 1);
    } else {
        err_capture_task(g_rec.task_name, sizeof(g_rec.task_name));
    }
    g_rec.task_name[ERR_TASK_NAME_MAX - 1] = '\0';
    strncpy(g_rec.cause, "FreeRTOS stack overflow detected",
            sizeof(g_rec.cause) - 1);
    g_rec.crc = err_bkp_crc((uint32_t *)&g_rec, sizeof(g_rec) / 4u);

    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, g_rec.seq);
    err_bkp_save(&g_rec);
    ERR_DumpRecord(&g_rec);
    err_recover();
}

void ERR_HandleTaskStall(const char *task_name, uint32_t silent_ms)
{
    __disable_irq();
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.magic = ERR_RECORD_MAGIC;
    g_rec.src = ERR_SRC_TASK_STALL;
    g_rec.seq = err_next_seq();
    g_rec.tick_ms = ERR_TickMs;
    if (task_name != NULL) {
        strncpy(g_rec.task_name, task_name, ERR_TASK_NAME_MAX - 1);
    } else {
        err_capture_task(g_rec.task_name, sizeof(g_rec.task_name));
    }
    g_rec.task_name[ERR_TASK_NAME_MAX - 1] = '\0';
    snprintf(g_rec.cause, sizeof(g_rec.cause),
             "Task watchdog timeout: %lu ms silent", (unsigned long)silent_ms);
    g_rec.crc = err_bkp_crc((uint32_t *)&g_rec, sizeof(g_rec) / 4u);

    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, g_rec.seq);
    err_bkp_save(&g_rec);
    ERR_DumpRecord(&g_rec);
    err_recover();
}

void ERR_HandleUnhandledIRQ(uint32_t irqn)
{
    __disable_irq();
    err_uart_puts("[ERR] unhandled-irq entry\r\n");   /* 入口标记：区分中断直接调用 */
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.magic = ERR_RECORD_MAGIC;
    g_rec.src = ERR_SRC_UNHANDLED_IRQ;
    g_rec.seq = err_next_seq();
    g_rec.tick_ms = ERR_TickMs;
    g_rec.pc = (uint32_t)(((uint32_t *)__get_MSP())[6]);
    err_capture_task(g_rec.task_name, sizeof(g_rec.task_name));
    snprintf(g_rec.cause, sizeof(g_rec.cause),
             "Unhandled interrupt, IRQn=%lu", (unsigned long)irqn);
    g_rec.msp = (uint32_t)__get_MSP();
    g_rec.psp = (uint32_t)__get_PSP();
    g_rec.call_depth = err_backtrace(g_rec.msp, g_rec.lr, g_rec.pc,
                                     g_rec.call_stack, ERR_CALL_STACK_MAX);
    g_rec.crc = err_bkp_crc((uint32_t *)&g_rec, sizeof(g_rec) / 4u);

    BSP_RTC_WriteBackupReg(ERR_BKP_SEQ_REG, g_rec.seq);
    err_bkp_save(&g_rec);
    ERR_DumpRecord(&g_rec);
    err_recover();
}

/* ---------- 启动复现 ---------- */
void ERR_ReportLastCrash(void)
{
    uint32_t magic = BSP_RTC_ReadBackupReg(ERR_BKP_MAGIC_REG);
    if ((magic & 0xFF) != (ERR_BKP_MAGIC_VAL & 0xFF)) {
        return;   /* 无有效崩溃记录*/
    }

    err_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = ERR_RECORD_MAGIC;
    rec.src = BSP_RTC_ReadBackupReg(ERR_BKP_SRC_REG);
    rec.seq = BSP_RTC_ReadBackupReg(ERR_BKP_SEQ_REG);
    rec.pc = BSP_RTC_ReadBackupReg(ERR_BKP_PC_REG);
    rec.lr = BSP_RTC_ReadBackupReg(ERR_BKP_LR_REG);
    rec.fault_addr = BSP_RTC_ReadBackupReg(ERR_BKP_ADDR_REG);
    rec.cfsr = BSP_RTC_ReadBackupReg(ERR_BKP_CFSR_REG);
    rec.hfsr = BSP_RTC_ReadBackupReg(ERR_BKP_HFSR_REG);
    rec.tick_ms = BSP_RTC_ReadBackupReg(ERR_BKP_TICK_REG);
    uint32_t name0 = BSP_RTC_ReadBackupReg(ERR_BKP_NAME_REG0);
    uint32_t name1 = BSP_RTC_ReadBackupReg(ERR_BKP_NAME_REG1);
    uint32_t name2 = BSP_RTC_ReadBackupReg(ERR_BKP_NAME_REG2);
    memcpy(rec.task_name, &name0, 4);
    memcpy(rec.task_name + 4, &name1, 4);
    memcpy(rec.task_name + 8, &name2, 4);
    rec.task_name[ERR_TASK_NAME_MAX - 1] = '\0';
    err_decode_cause((err_src_t)rec.src, rec.cfsr, rec.hfsr,
                     rec.cause, sizeof(rec.cause));
    rec.call_depth = 0;

    LOG_Printf("\r\n[CRASH] Previous crash recovered: #%lu, %s, uptime=%lu ms\r\n",
               (unsigned long)rec.seq,
               err_src_name((err_src_t)rec.src),
               (unsigned long)rec.tick_ms);
    LOG_Printf("[CRASH]   PC=");
    /* LOG_Printf 不支持 %X 的高位补零？直接打印十六进制 */
    LOG_Printf("%08lX", (unsigned long)rec.pc);
    LOG_Printf("  LR=%08lX  cause=%s\r\n",
               (unsigned long)rec.lr, rec.cause);
    if (rec.fault_addr != 0) {
        LOG_Printf("[CRASH]   Fault addr=%08lX\r\n",
                   (unsigned long)rec.fault_addr);
    }
    if (rec.task_name[0]) {
        LOG_Printf("[CRASH]   Task=%s\r\n", rec.task_name);
    }
}

uint32_t ERR_GetCrashSeq(void)
{
    return BSP_RTC_ReadBackupReg(ERR_BKP_SEQ_REG);
}

const err_record_t *ERR_GetLastRecord(void)
{
    return (g_rec.magic == ERR_RECORD_MAGIC) ? &g_rec : NULL;
}

void ERR_Init(void)
{
    memset(&g_rec, 0, sizeof(g_rec));
    g_locked = 0;

    /* 清理早期调试布局的历史残留：崩溃序号异常（>100000）时清空摘要，     * 保证重启后崩溃序号从 1 重新计数（BKP 布局版本迁移保护）。*/
    if (BSP_RTC_ReadBackupReg(ERR_BKP_SEQ_REG) > 100000u) {
        for (uint32_t i = ERR_BKP_MAGIC_REG; i <= ERR_BKP_RTC_REG; i++) {
            BSP_RTC_WriteBackupReg(i, 0);
        }
    }
}
