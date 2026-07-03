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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_VALID_MAGIC       0x4F54412E  // ".OTA" 魔数，随意定义

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
  // MX_IWDG_Init();
  MX_RTC_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 启动独立看门狗 */
  MX_IWDG_Start();

  /* 初始化日志 */
  Log_Init();
  printf("BOOT Started.\r\n");

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
static uint8_t CheckAppValid(uint32_t addr)
{
    uint32_t magic = *((volatile uint32_t *)(addr + 4));   // 假设头 4 字节是栈顶，后 4 字节是魔数
    if (magic == APP_VALID_MAGIC)
    {
        // 可进一步添加 CRC32 校验头部（暂略）
        return 1;
    }
    return 0;
}
/**
  * @brief  跳转到 APP 固件
  * @param  addr: APP 基地址（必须为 0x08010000 对齐）
  */
static void JumpToApp(uint32_t addr)
{
    uint32_t app_stack = *((volatile uint32_t *)addr);      // 首 4 字节是 MSP 初始值
    uint32_t app_reset = *((volatile uint32_t *)(addr + 4));// 次 4 字节是复位向量（即入口地址）

    /* 关闭所有外设中断（避免跳转后触发未初始化中断） */
    __disable_irq();

    /* 关闭所有可能在 BOOT 中开启的外设（可选，视情况） */
    // HAL_DeInit(); 等

    /* 设置向量表偏移 */
    SCB->VTOR = addr;

    /* 设置主栈指针 */
    __set_MSP(app_stack);

    /* 跳转到 APP 复位处理函数 */
    ((void (*)(void))app_reset)();
}
/**
  * @brief  进入升级模式（等待接收固件）
  */
static void EnterUpgradeMode(void)
{
    printf("Entering upgrade mode...\r\n");

    ymodem_t ctx;
    ymodem_status_t status;

    /* 调用Ymodem接收，将数据起始地址设为 DOWNLOAD_BASE_ADDR（暂不写Flash） */
    status = ymodem_receive_file(&ctx, DOWNLOAD_BASE_ADDR);

    if (status == YMODEM_OK) {
        printf("Upgrade file received successfully!\r\n");
        // 未来这里进行解密、验签、烧写
        // 清除升级标志，跳转APP
        BKP_WRITE(RTC_BKP_DR1, BOOT_FLAG_NONE);
        printf("Rebooting to APP...\r\n");
        HAL_Delay(100);
        NVIC_SystemReset();
    } else {
        printf("Upgrade failed with code: %d\r\n", status);
        // 停留在此，等待再次升级
    }

    while (1) {
        HAL_IWDG_Refresh(&hiwdg);
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
