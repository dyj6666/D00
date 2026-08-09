/* ================================================================
 * GCC 工具链故障入口（naked asm）
 * 语义与 Keil 工程 stm32f4xx_it.c 中的 ARMCC __asm 包装完全一致：
 * 在进入 C 之前按 EXC_RETURN.bit2 选择栈指针（1=线程模式 PSP，
 * 0=处理模式 MSP），并传入 R0=栈帧指针 R1=EXC_RETURN R2=err_src_t。
 * 仅供 CMake/GCC 构建使用（Keil 工程不含本文件）。
 * ================================================================ */
#include "err_mgr.h"

__attribute__((naked, used)) void NMI_Handler(void)
{
    __asm volatile(
        "TST LR, #4\n"
        "ITE EQ\n"
        "MRSEQ R0, MSP\n"
        "MRSNE R0, PSP\n"
        "MOV R1, LR\n"
        "MOV R2, #1\n"
        "LDR R3, =ERR_HandleFaultEntry\n"
        "BX R3\n");
}

__attribute__((naked, used)) void HardFault_Handler(void)
{
    __asm volatile(
        "TST LR, #4\n"
        "ITE EQ\n"
        "MRSEQ R0, MSP\n"
        "MRSNE R0, PSP\n"
        "MOV R1, LR\n"
        "MOV R2, #2\n"
        "LDR R3, =ERR_HandleFaultEntry\n"
        "BX R3\n");
}

__attribute__((naked, used)) void MemManage_Handler(void)
{
    __asm volatile(
        "TST LR, #4\n"
        "ITE EQ\n"
        "MRSEQ R0, MSP\n"
        "MRSNE R0, PSP\n"
        "MOV R1, LR\n"
        "MOV R2, #3\n"
        "LDR R3, =ERR_HandleFaultEntry\n"
        "BX R3\n");
}

__attribute__((naked, used)) void BusFault_Handler(void)
{
    __asm volatile(
        "TST LR, #4\n"
        "ITE EQ\n"
        "MRSEQ R0, MSP\n"
        "MRSNE R0, PSP\n"
        "MOV R1, LR\n"
        "MOV R2, #4\n"
        "LDR R3, =ERR_HandleFaultEntry\n"
        "BX R3\n");
}

__attribute__((naked, used)) void UsageFault_Handler(void)
{
    __asm volatile(
        "TST LR, #4\n"
        "ITE EQ\n"
        "MRSEQ R0, MSP\n"
        "MRSNE R0, PSP\n"
        "MOV R1, LR\n"
        "MOV R2, #5\n"
        "LDR R3, =ERR_HandleFaultEntry\n"
        "BX R3\n");
}
