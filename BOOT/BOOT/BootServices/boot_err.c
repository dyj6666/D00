#include "boot_err.h"
#include "usart.h"
#include "rtc.h"
#include "stm32f4xx_hal.h"

#include <stdio.h>
#include <string.h>

/* ================================================================
 * BOOT 轻量纠错实现
 * ================================================================ */

/* ---------- 来源名称 ---------- */
static const char *boot_err_src_name(uint32_t src)
{
    switch (src) {
    case BOOT_ERR_NMI:        return "NMI";
    case BOOT_ERR_HARDFAULT:  return "HardFault";
    case BOOT_ERR_MEMMANAGE:  return "MemManage";
    case BOOT_ERR_BUSFAULT:   return "BusFault";
    case BOOT_ERR_USAGEFAULT: return "UsageFault";
    default:                  return "Unknown";
    }
}

/* ---------- 裸寄存器 UART2 输出（fault 上下文不依赖 printf/HAL） ---------- */
static void err_uart_putc(uint8_t c)
{
    while (!(huart2.Instance->SR & USART_SR_TXE)) {
    }
    huart2.Instance->DR = c;
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

/* ---------- fault 原因解码 ---------- */
static const char *fault_cause(uint32_t cfsr)
{
    if (cfsr & (1u << 25)) return "DIVBYZERO";
    if (cfsr & (1u << 24)) return "UNALIGNED";
    if (cfsr & (1u << 18)) return "INVPC";
    if (cfsr & (1u << 17)) return "INVSTATE";
    if (cfsr & (1u << 16)) return "UNDEFINSTR";
    if (cfsr & (1u << 10)) return "IMPRECISERR";
    if (cfsr & (1u << 9))  return "PRECISERR";
    if (cfsr & (1u << 8))  return "IBUSERR";
    if (cfsr & (1u << 1))  return "DACCVIOL";
    if (cfsr & (1u << 0))  return "IACCVIOL";
    return "UNKNOWN";
}

/* ---------- BKP 摘要 ---------- */
static uint32_t bkp_crc(uint32_t a, uint32_t b, uint32_t c)
{
    return 0x5A5A5A5Au ^ (a ^ ((b << 1) | (b >> 31))) ^ (c ^ ((c << 2) | (c >> 30)));
}

/* ---------- 统一 fault 入口 ---------- */
void Boot_ErrFaultEntry(uint32_t *stack_frame, uint32_t exc_return,
                        uint32_t src)
{
    __disable_irq();

    /* 中止 USART2 TX DMA（不依赖 HAL，防中断上下文阻塞） */
    if (huart2.hdmatx != NULL) {
        DMA_Stream_TypeDef *tx = huart2.hdmatx->Instance;
        tx->CR &= ~DMA_SxCR_EN;
        DMA_TypeDef *dma = (huart2.hdmatx->StreamIndex < 8) ? DMA1 : DMA2;
        dma->LIFCR = 0xFFFFFFFFu;
        dma->HIFCR = 0xFFFFFFFFu;
    }
    CLEAR_BIT(huart2.Instance->CR3, USART_CR3_DMAT);

    uint32_t pc = stack_frame[6];
    uint32_t lr = stack_frame[5];
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    uint32_t msp = (uint32_t)__get_MSP();
    uint32_t psp = (uint32_t)__get_PSP();

    err_uart_puts("\r\n===========================================================\r\n");
    err_uart_puts("        BOOT FAULT DIAGNOSTIC REPORT\r\n");
    err_uart_puts("===========================================================\r\n");
    err_uart_puts("Source   : ");
    err_uart_puts(boot_err_src_name(src));
    err_uart_puts(" (");
    err_uart_puts(fault_cause(cfsr));
    err_uart_puts(")\r\n");
    err_uart_puts("PC       : "); err_uart_hex32(pc); err_uart_puts("\r\n");
    err_uart_puts("LR       : "); err_uart_hex32(lr); err_uart_puts("\r\n");
    err_uart_puts("xPSR     : "); err_uart_hex32(stack_frame[7]); err_uart_puts("\r\n");
    err_uart_puts("EXC_RET  : "); err_uart_hex32(exc_return); err_uart_puts("\r\n");
    err_uart_puts("CFSR     : "); err_uart_hex32(cfsr); err_uart_puts("\r\n");
    err_uart_puts("HFSR     : "); err_uart_hex32(hfsr); err_uart_puts("\r\n");
    err_uart_puts("R0-R3    : ");
    err_uart_hex32(stack_frame[0]); err_uart_puts(" ");
    err_uart_hex32(stack_frame[1]); err_uart_puts(" ");
    err_uart_hex32(stack_frame[2]); err_uart_puts(" ");
    err_uart_hex32(stack_frame[3]); err_uart_puts("\r\n");
    err_uart_puts("MSP      : "); err_uart_hex32(msp); err_uart_puts("\r\n");
    err_uart_puts("PSP      : "); err_uart_hex32(psp); err_uart_puts("\r\n");
    err_uart_puts("===========================================================\r\n");

    /* 摘要持久化（reg 16-19，BOOT 专用区） */
    uint32_t seq = (HAL_RTCEx_BKUPRead(&hrtc, BOOT_ERR_BKP_SRCSEQ_REG) >> 8) + 1u;
    HAL_RTCEx_BKUPWrite(&hrtc, BOOT_ERR_BKP_MAGIC_REG, BOOT_ERR_BKP_MAGIC_VAL);
    HAL_RTCEx_BKUPWrite(&hrtc, BOOT_ERR_BKP_SRCSEQ_REG,
                        (src & 0xFFu) | ((seq & 0xFFFFFFu) << 8));
    HAL_RTCEx_BKUPWrite(&hrtc, BOOT_ERR_BKP_PC_REG, pc);
    HAL_RTCEx_BKUPWrite(&hrtc, BOOT_ERR_BKP_CRC_REG,
                        bkp_crc(BOOT_ERR_BKP_MAGIC_VAL,
                                (src & 0xFFu) | ((seq & 0xFFFFFFu) << 8), pc));

    /* 软复位恢复（绝不死循环）；IWDG 在 BOOT 启动路径持续兜底 */
    NVIC_SystemReset();
    for (;;) {
    }
}

/* ---------- 启动复现 ---------- */
void Boot_ErrReportLast(void)
{
    uint32_t magic = HAL_RTCEx_BKUPRead(&hrtc, BOOT_ERR_BKP_MAGIC_REG);
    if (magic != BOOT_ERR_BKP_MAGIC_VAL) {
        return;
    }
    uint32_t srcseq = HAL_RTCEx_BKUPRead(&hrtc, BOOT_ERR_BKP_SRCSEQ_REG);
    uint32_t pc = HAL_RTCEx_BKUPRead(&hrtc, BOOT_ERR_BKP_PC_REG);
    uint32_t crc = HAL_RTCEx_BKUPRead(&hrtc, BOOT_ERR_BKP_CRC_REG);
    if (crc != bkp_crc(BOOT_ERR_BKP_MAGIC_VAL, srcseq, pc)) {
        return;   /* 摘要损坏，忽略 */
    }
    printf("\r\n[BOOT-CRASH] Previous BOOT fault recovered: %s, "
           "PC=0x%08lX, seq=%lu\r\n",
           boot_err_src_name(srcseq & 0xFFu),
           (unsigned long)pc,
           (unsigned long)(srcseq >> 8));
}
