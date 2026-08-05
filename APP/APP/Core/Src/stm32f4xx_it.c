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
void hardfault_dump_c(uint32_t *stack_frame);

/* 仿照 CMSIS 写法，手动实现 __get_LR */
__STATIC_INLINE uint32_t __get_LR(void)
{
  register uint32_t __regLinkRegister  __ASM("lr");
  return(__regLinkRegister);
}

/* 诊断函数（保持不变，但MSP/PSP统一使用CMSIS安全函数） */
static const char *fault_type_str(uint32_t cfsr) {
    if (cfsr & (1 << 25)) return "DIVBY0 (Divide by zero)";
    if (cfsr & (1 << 24)) return "UNALIGNED";
    if (cfsr & (1 << 17)) return "INVSTATE (Invalid state)";
    if (cfsr & (1 << 16)) return "UNDEFINSTR";
    if (cfsr & (1 << 9))  return "PRECISERR (Precise data bus error)";
    if (cfsr & (1 << 8))  return "IMPRECISERR (Imprecise data bus error)";
    if (cfsr & (1 << 1))  return "DACCVIOL (Data access violation)";
    if (cfsr & (1 << 0))  return "IACCVIOL (Instruction access violation)";
    return "UNKNOWN";
}

static void hardfault_dump(uint32_t *stack_frame) {
    char buf[256];
    uint32_t stacked_r0 = stack_frame[0];
    uint32_t stacked_r1 = stack_frame[1];
    uint32_t stacked_r2 = stack_frame[2];
    uint32_t stacked_r3 = stack_frame[3];
    uint32_t stacked_r12 = stack_frame[4];
    uint32_t stacked_lr  = stack_frame[5];
    uint32_t stacked_pc  = stack_frame[6];
    uint32_t stacked_psr = stack_frame[7];

    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    uint32_t bfar = SCB->BFAR;
    uint32_t mmfar = SCB->MMFAR;

    int len = sprintf(buf, "\r\n===== HARD FAULT =====\r\n");
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    len = sprintf(buf, "CFSR: 0x%08X  HFSR: 0x%08X\r\n", (unsigned int)cfsr, (unsigned int)hfsr);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    if (hfsr & (1 << 30)) {
        len = sprintf(buf, "FORCED HardFault (from another exception)\r\n");
        HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);
    }

    const char *type = fault_type_str(cfsr);
    len = sprintf(buf, "Fault Type: %s\r\n", type);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    if (cfsr & (1 << 7)) {
        len = sprintf(buf, "Fault Address (BFAR): 0x%08X\r\n", (unsigned int)bfar);
        HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);
    }
    if (cfsr & (1 << 7)) {
        len = sprintf(buf, "Fault Address (MMFAR): 0x%08X\r\n", (unsigned int)mmfar);
        HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);
    }

    len = sprintf(buf, "R0: 0x%08X  R1: 0x%08X  R2: 0x%08X  R3: 0x%08X\r\n",
                  (unsigned int)stacked_r0, (unsigned int)stacked_r1,
                  (unsigned int)stacked_r2, (unsigned int)stacked_r3);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    len = sprintf(buf, "R12: 0x%08X  LR: 0x%08X  PC: 0x%08X  xPSR: 0x%08X\r\n",
                  (unsigned int)stacked_r12, (unsigned int)stacked_lr,
                  (unsigned int)stacked_pc, (unsigned int)stacked_psr);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    /* MSP 和 PSP 使用 CMSIS 安全函数 */
    uint32_t msp_val = __get_MSP();
    uint32_t psp_val = __get_PSP();
    len = sprintf(buf, "MSP: 0x%08X  PSP: 0x%08X\r\n", (unsigned int)msp_val, (unsigned int)psp_val);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);

    len = sprintf(buf, "PC (instruction): 0x%08X\r\n", (unsigned int)stacked_pc);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);
    len = sprintf(buf, "LR (return): 0x%08X\r\n", (unsigned int)stacked_lr);
    HAL_UART_Transmit(&DEBUG_UART, (uint8_t*)buf, len, 1000);
}

void hardfault_dump_c(uint32_t *stack_frame) {
    hardfault_dump(stack_frame);
    while (1) {
        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
        for (volatile int i = 0; i < 500000; i++);
    }
}
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_tim3_ch4_up;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;
extern DMA_HandleTypeDef hdma_tim1_up;
extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart2_tx;
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern TIM_HandleTypeDef htim7;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  uint32_t *stack_ptr;

    /* 使用自定义 __get_LR 和 CMSIS 标准函数 */
    if (__get_LR() & 0x04) {
        stack_ptr = (uint32_t *)__get_PSP();
    } else {
        stack_ptr = (uint32_t *)__get_MSP();
    }

    hardfault_dump_c(stack_ptr);
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
    HAL_Delay(100);
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

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
  * @brief This function handles DMA1 stream5 global interrupt.
  */
void DMA1_Stream5_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream5_IRQn 0 */

  /* USER CODE END DMA1_Stream5_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart2_rx);
  /* USER CODE BEGIN DMA1_Stream5_IRQn 1 */

  /* USER CODE END DMA1_Stream5_IRQn 1 */
}

/**
  * @brief This function handles DMA1 stream6 global interrupt.
  */
void DMA1_Stream6_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream6_IRQn 0 */

  /* USER CODE END DMA1_Stream6_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart2_tx);
  /* USER CODE BEGIN DMA1_Stream6_IRQn 1 */

  /* USER CODE END DMA1_Stream6_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[9:5] interrupts.
  */
void EXTI9_5_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_5_IRQn 0 */

  /* USER CODE END EXTI9_5_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
  /* USER CODE BEGIN EXTI9_5_IRQn 1 */

  /* USER CODE END EXTI9_5_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[15:10] interrupts.
  */
void EXTI15_10_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI15_10_IRQn 0 */

  /* USER CODE END EXTI15_10_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_12);
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);
  /* USER CODE BEGIN EXTI15_10_IRQn 1 */

  /* USER CODE END EXTI15_10_IRQn 1 */
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
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
  /* USER CODE BEGIN USART2_IRQn 0 */

  /* USER CODE END USART2_IRQn 0 */
  HAL_UART_IRQHandler(&huart2);
  /* USER CODE BEGIN USART2_IRQn 1 */
  BSP_UART_IdleISR(BSP_UART_DBG);
  /* USER CODE END USART2_IRQn 1 */
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

/* USER CODE END 1 */
