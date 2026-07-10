/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "dma.h"
#include "iwdg.h"
#include "rtc.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "boot_config.h"
#include <string.h>
#include <stdio.h>                    // 后续 log 用
#include "ymodem.h"
#include "flash_if.h"
#include "security.h"
#include "uECC.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 备份域访问宏，简化书写 */
#define BKP_READ(reg)   HAL_RTCEx_BKUPRead(&hrtc, (reg))
#define BKP_WRITE(reg, val) HAL_RTCEx_BKUPWrite(&hrtc, (reg), (val))
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 私有变量 */
static volatile uint8_t upgrade_flag = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void MX_IWDG_Start(void);        // 启动独立看门狗
static void Log_Init(void);             // 初始化日志串口
static uint8_t CheckAppValid(uint32_t addr);
static void JumpToApp(uint32_t addr);
static void EnterUpgradeMode(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_IWDG_Init();
  MX_RTC_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 启动独立看门狗 */
  MX_IWDG_Start();

  /* 初始化日志 */
  Log_Init();
  printf("BOOT Started.\r\n");
  /* 诊断：打印复位源 */
  uint32_t reset_flags = RCC->CSR;
  printf("RCC_CSR: 0x%08X\r\n", (unsigned int)reset_flags);
  if (reset_flags & RCC_CSR_IWDGRSTF) {
      printf("Reset Source: IWDG\r\n");
  }
  if (reset_flags & RCC_CSR_SFTRSTF) {
      printf("Reset Source: Software\r\n");
  }
  if (reset_flags & RCC_CSR_PORRSTF) {
      printf("Reset Source: Power-on\r\n");
  }
  if (reset_flags & RCC_CSR_PINRSTF) {
      printf("Reset Source: Pin reset\r\n");
  }
  // 清除复位标志
  RCC->CSR |= RCC_CSR_RMVF;

    /* 检查升级标志 */
  if (BKP_READ(RTC_BKP_DR1) == BOOT_FLAG_UPGRADE)
  {
      printf("Upgrade flag set. Entering upgrade mode.\r\n");
      EnterUpgradeMode();
  }
  else
  {
      /* 检查 APP 是否有效 */
      if (CheckAppValid(APP_BASE_ADDR))
      {
          printf("APP valid, jumping to APP...\r\n");
          JumpToApp(APP_BASE_ADDR);
      }
      else
      {
          printf("APP invalid, entering upgrade mode.\r\n");
          EnterUpgradeMode();
      }
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);   // 一旦升级模式退出，我们仍喂狗防止复位

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/**
  * @brief  启动独立看门狗（一旦启动，无法软件关闭，直至复位）
  */
static void MX_IWDG_Start(void)
{
    /* 使能备份域和电源接口时钟（需要用到时已由 RTC 初始化做了，这里可省略）*/
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* 初始化并启动 IWDG */
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER;
    hiwdg.Init.Reload = IWDG_RELOAD;
    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
    {
        // 初始化失败通常是因为已经启动，我们直接喂狗即可
        HAL_IWDG_Refresh(&hiwdg);
    }
}
/**
  * @brief  初始化日志串口（USART2, 115200-8-N-1）
  */
static void Log_Init(void)
{
    // CubeMX 已经生成 MX_USART2_UART_Init()，所以无需额外初始化
    // 如果需要 printf 重定向，可在 usart.c 中实现 fputc，此处先留空
}
/**
  * @brief  检查 APP 固件有效性
  * @param  addr: APP 基地址
  * @retval 1 有效，0 无效
  */
static uint8_t CheckAppValid(uint32_t addr) {
    uint32_t magic = *((volatile uint32_t *)(addr + APP_VALID_OFFSET));
    if (magic == APP_VALID_MAGIC) {
        return 1;
    }
    return 0;
}
/**
  * @brief  跳转到 APP 固件
  * @param  addr: APP 基地址（必须为 0x08010000 对齐）
  */
static void JumpToApp(uint32_t addr) {
    uint32_t app_stack = *(volatile uint32_t *)addr;
    uint32_t app_reset = *(volatile uint32_t *)(addr + 4);

    printf("Jumping to APP: SP=0x%08X, PC=0x%08X\r\n", app_stack, app_reset);

    /* 1. 彻底关闭 USART2 的发送和接收，清除所有状态 */
    HAL_UART_Abort(&huart2);             /* 中止所有传输并等待完成 */
    HAL_UART_DeInit(&huart2);            /* 反初始化，包括调用 MspDeInit 关闭时钟和中断 */

    /* 2. 强制复位 USART2 外设内核 (确保寄存器回到复位值) */
    __HAL_RCC_USART2_FORCE_RESET();
    __HAL_RCC_USART2_RELEASE_RESET();
    
    /* 喂狗 */
    IWDG->KR = 0xAAAA;

    /* 关闭全局中断并清除所有挂起的中断 */
    __disable_irq();

    /* 复位 SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* 清除中断挂起寄存器 */
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;   /* 关闭所有中断使能 */
        NVIC->ICPR[i] = 0xFFFFFFFF;   /* 清除所有挂起中断 */
    }

    /* 设置向量表 */
    SCB->VTOR = addr;
    __DSB();
    __ISB();

    __set_MSP(app_stack);
    __DSB();
    __ISB();

    ((void (*)(void))app_reset)();
}
/**
  * @brief  进入升级模式（等待接收固件）
  */
static void EnterUpgradeMode(void) {
    printf("Entering upgrade mode (top-tier FSM)...\r\n");

    ymodem_port_init();

    /* 擦除 Download 区 */
    printf("Erasing Download area...\r\n");
    if (!flash_erase(DOWNLOAD_BASE_ADDR, DOWNLOAD_BASE_ADDR + DOWNLOAD_SIZE - 1)) {
        printf("Download erase failed!\r\n");
        while (1) { IWDG->KR = 0xAAAA; }
    }
    /* 验证擦除 */
    /* 擦除后验证 */
    uint32_t test_word = *(volatile uint32_t *)DOWNLOAD_BASE_ADDR;
    printf("After Download Erase, first word: 0x%08X\r\n", (unsigned)test_word);
    if (test_word != 0xFFFFFFFF) {
        printf("ERROR: Download not fully erased!\r\n");
        while (1) { IWDG->KR = 0xAAAA; }
    }

    ymodem_ctx_t ctx;
    ymodem_status_t status = ymodem_receive(&ctx, DOWNLOAD_BASE_ADDR);

    if (status == YMODEM_OK) {
        printf("OTA success. File: %s, Size: %lu\r\n", ctx.file_name, ctx.received_size);

        // 打印 Download 区前 32 字节（用于比对）
        printf("Download first 32 bytes: ");
        for (int i = 0; i < 32; i++) {
            printf("%02X ", *((volatile uint8_t *)(DOWNLOAD_BASE_ADDR + i)));
        }
        printf("\r\n");

        /* 安全处理前，先擦除 APP 区 */
        printf("Erasing APP area...\r\n");
        if (!flash_erase(APP_BASE_ADDR, APP_BASE_ADDR + APP_SIZE - 1)) {
            printf("APP erase failed!\r\n");
            while (1) { IWDG->KR = 0xAAAA; }
        }

        uint32_t app_size = 0;
        if (security_verify_and_decrypt(DOWNLOAD_BASE_ADDR, APP_BASE_ADDR, &app_size)) {
            printf("Security verification passed. Writing magic...\r\n");
            uint32_t magic = APP_VALID_MAGIC;
            flash_write(APP_VALID_ADDR, (uint8_t *)&magic, sizeof(magic));

            BKP_WRITE(RTC_BKP_DR1, BOOT_FLAG_NONE);
            printf("Update successful! Rebooting to new APP...\r\n");
            HAL_Delay(100);
            NVIC_SystemReset();
        } else {
            printf("Security verification failed!\r\n");
        }
    } else {
        printf("OTA failed, status: %d\r\n", status);
    }

    while (1) {
        IWDG->KR = 0xAAAA;
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
