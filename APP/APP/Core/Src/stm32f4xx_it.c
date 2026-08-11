/* ================================================================
 * stm32f4xx_it —— 中断服务例程
 *
 * 架构位置：APP 入口；SysTick/外设中断集中处理
 * ================================================================ */
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bsp.h"
#include "pinout.h"
#include <stdio.h>
#include <string.h>
#include "stdint.h"
#include "stm32f4xx_hal.h"
#include "core_cm4.h"
#include "la_sample.h"
#include "signal_gen.h"
#include "err_mgr.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* GCC 工具链不使用 ARMCC 内联汇编包装：5 个故障入口由
 * gcc_port/fault_handlers_gcc.c 提供等价 naked asm 版本 */
#if !defined(__GNUC__)
/* Note: The 5 fault entry wrappers below (NMI/HardFault/MemManage/BusFault/
 * UsageFault) are the assembly front-ends of the err_mgr diagnostic system;
 * they must capture the real stack pointer before entering C.
 * CubeMX regeneration re-adds 5 default __weak C handlers with the same
 * names -> duplicate definition errors (#247). Remove those default C
 * handlers after each CubeMX run (see Script/fix_after_mx.ps1).
 */
/* 异常入口汇编包装：在进入 C 之前捕获真实 EXC_RETURN，
 * 并按 EXC_RETURN.bit2 选择栈指针（1=线程模式 PSP，0=处理模式 MSP）。
 * 传递：R0=栈帧指针，R1=EXC_RETURN，R2=错误来源（err_src_t 枚举值）。 */
__asm void NMI_Handler(void)
{
    IMPORT ERR_HandleFaultEntry
    TST  LR, #4
    ITE  EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV  R1, LR
    MOV  R2, #1
    LDR  R3, =ERR_HandleFaultEntry
    BX   R3
}

__asm void HardFault_Handler(void)
{
    IMPORT ERR_HandleFaultEntry
    TST  LR, #4
    ITE  EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV  R1, LR
    MOV  R2, #2
    LDR  R3, =ERR_HandleFaultEntry
    BX   R3
}

__asm void MemManage_Handler(void)
{
    IMPORT ERR_HandleFaultEntry
    TST  LR, #4
    ITE  EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV  R1, LR
    MOV  R2, #3
    LDR  R3, =ERR_HandleFaultEntry
    BX   R3
}

__asm void BusFault_Handler(void)
{
    IMPORT ERR_HandleFaultEntry
    TST  LR, #4
    ITE  EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV  R1, LR
    MOV  R2, #4
    LDR  R3, =ERR_HandleFaultEntry
    BX   R3
}

__asm void UsageFault_Handler(void)
{
    IMPORT ERR_HandleFaultEntry
    TST  LR, #4
    ITE  EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV  R1, LR
    MOV  R2, #5
    LDR  R3, =ERR_HandleFaultEntry
    BX   R3
}
#endif
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
extern DMA_HandleTypeDef hdma_tim1_up;
extern DMA_HandleTypeDef hdma_tim3_ch4_up;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern DMA_HandleTypeDef hdma_usart3_tx;
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart3;
extern TIM_HandleTypeDef htim7;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 stream1 global interrupt.
  */
void DMA1_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream1_IRQn 0 */

  /* USER CODE END DMA1_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
  /* USER CODE BEGIN DMA1_Stream1_IRQn 1 */

  /* USER CODE END DMA1_Stream1_IRQn 1 */
}

/**
  * @brief This function handles DMA1 stream2 global interrupt.
  */
void DMA1_Stream2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream2_IRQn 0 */

  /* USER CODE END DMA1_Stream2_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_tim3_ch4_up);
  /* USER CODE BEGIN DMA1_Stream2_IRQn 1 */

  /* USER CODE END DMA1_Stream2_IRQn 1 */
}

/**
  * @brief This function handles DMA1 stream3 global interrupt.
  */
void DMA1_Stream3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream3_IRQn 0 */

  /* USER CODE END DMA1_Stream3_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_tx);
  /* USER CODE BEGIN DMA1_Stream3_IRQn 1 */

  /* USER CODE END DMA1_Stream3_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[9:5] interrupts.
  */
void EXTI9_5_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_5_IRQn 0 */

  /* USER CODE END EXTI9_5_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);   /* LA CH0 = PG6 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);   /* LA CH1 = PG7 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);   /* LA CH2 = PG8 */
  /* USER CODE BEGIN EXTI9_5_IRQn 1 */

  /* USER CODE END EXTI9_5_IRQn 1 */
}


/**
  * @brief EXTI15_10：LA CH3 = PG15 时间戳触发
  */
void EXTI15_10_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);  /* LA CH3 = PG15 */
}


/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */
  LA_Timestamp_Overflow_Handler();
  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles TIM3 global interrupt.
  */
void TIM3_IRQHandler(void)
{
  /* USER CODE BEGIN TIM3_IRQn 0 */

  /* USER CODE END TIM3_IRQn 0 */
  HAL_TIM_IRQHandler(&htim3);
  /* USER CODE BEGIN TIM3_IRQn 1 */
  /* USER CODE END TIM3_IRQn 1 */
}

/**
  * @brief This function handles USART1 global interrupt.
  */
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */

  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
  /* USER CODE BEGIN USART1_IRQn 1 */
  BSP_UART_IdleISR(BSP_UART_HOST);

  /* USER CODE END USART1_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */

  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */
  BSP_UART_IdleISR(BSP_UART_DBG);
  /* USER CODE END USART3_IRQn 1 */
}

/**
  * @brief This function handles TIM7 global interrupt.
  */
void TIM7_IRQHandler(void)
{
  /* USER CODE BEGIN TIM7_IRQn 0 */

  /* USER CODE END TIM7_IRQn 0 */
  HAL_TIM_IRQHandler(&htim7);
  /* USER CODE BEGIN TIM7_IRQn 1 */

  /* USER CODE END TIM7_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream2 global interrupt.
  */
void DMA2_Stream2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream2_IRQn 0 */

  /* USER CODE END DMA2_Stream2_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart1_rx);
  /* USER CODE BEGIN DMA2_Stream2_IRQn 1 */

  /* USER CODE END DMA2_Stream2_IRQn 1 */
}

/**
  * @brief This function handles Ethernet global interrupt.
  */
void ETH_IRQHandler(void)
{
  /* USER CODE BEGIN ETH_IRQn 0 */

  /* USER CODE END ETH_IRQn 0 */
  HAL_ETH_IRQHandler(&heth);
  /* USER CODE BEGIN ETH_IRQn 1 */

  /* USER CODE END ETH_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream5 global interrupt.
  */
void DMA2_Stream5_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream5_IRQn 0 */

  /* USER CODE END DMA2_Stream5_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_tim1_up);
  /* USER CODE BEGIN DMA2_Stream5_IRQn 1 */

  /* USER CODE END DMA2_Stream5_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream7 global interrupt.
  */
void DMA2_Stream7_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream7_IRQn 0 */

  /* USER CODE END DMA2_Stream7_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart1_tx);
  /* USER CODE BEGIN DMA2_Stream7_IRQn 1 */

  /* USER CODE END DMA2_Stream7_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/**
  * @brief DMA2 stream6：信号发生器 USART6 TX DMA（运行期自建，非 CubeMX 模型）。
  *        放在 USER CODE 区，CubeMX 重新生成时不会被清除。
  */
void DMA2_Stream6_IRQHandler(void)
{
  SG_Uart_DMA_IRQHandler();
}

/* USER CODE END 1 */
