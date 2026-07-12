/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "logger.h"
#include "shell.h"
#include "app_config.h"
#include "stream_buffer.h"
#include "event_bus.h"
#include "timers.h"
#include "module.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for startupTask */
osThreadId_t startupTaskHandle;
const osThreadAttr_t startupTask_attributes = {
  .name = "startupTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for shellTask */
osThreadId_t shellTaskHandle;
const osThreadAttr_t shellTask_attributes = {
  .name = "shellTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for loggerTXTask */
osThreadId_t loggerTXTaskHandle;
const osThreadAttr_t loggerTXTask_attributes = {
  .name = "loggerTXTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for eventBusTask */
osThreadId_t eventBusTaskHandle;
const osThreadAttr_t eventBusTask_attributes = {
  .name = "eventBusTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityRealtime,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void tmr_1s_callback(TimerHandle_t xTimer) {
    EventBus_Publish(EVENT_TICK_1S, NULL, 0);
}
static void tmr_200ms_callback(TimerHandle_t xTimer) {
    EventBus_Publish(EVENT_TICK_200MS, NULL, 0);
}
/* USER CODE END FunctionPrototypes */

void StartStartupTask(void *argument);
void StartShellTask(void *argument);
void StartLoggerTXTask(void *argument);
void StartEventBusTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void configureTimerForRunTimeStats(void);
unsigned long getRunTimeCounterValue(void);
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);

/* USER CODE BEGIN 1 */
/* Functions needed when configGENERATE_RUN_TIME_STATS is on */
__weak void configureTimerForRunTimeStats(void)
{

}

__weak unsigned long getRunTimeCounterValue(void)
{
return 0;
}
/* USER CODE END 1 */

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
}
/* USER CODE END 4 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of startupTask */
  startupTaskHandle = osThreadNew(StartStartupTask, NULL, &startupTask_attributes);

  /* creation of shellTask */
  shellTaskHandle = osThreadNew(StartShellTask, NULL, &shellTask_attributes);

  /* creation of loggerTXTask */
  loggerTXTaskHandle = osThreadNew(StartLoggerTXTask, NULL, &loggerTXTask_attributes);

  /* creation of eventBusTask */
  eventBusTaskHandle = osThreadNew(StartEventBusTask, NULL, &eventBusTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */

  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartStartupTask */
/**
  * @brief  Function implementing the startupTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartStartupTask */
void StartStartupTask(void *argument)
{
  /* USER CODE BEGIN StartStartupTask */
  LOG_Init();
  // modules_init();   // 自动加载所有注册的模块

  // 系统定时器仍发布事件，但现在是非阻塞异步发布
  TimerHandle_t tmr_1s = xTimerCreate("t1s", pdMS_TO_TICKS(1000), pdTRUE, NULL, tmr_1s_callback);
  TimerHandle_t tmr_200ms = xTimerCreate("t200ms", pdMS_TO_TICKS(200), pdTRUE, NULL, tmr_200ms_callback);
  xTimerStart(tmr_1s, 0);
  xTimerStart(tmr_200ms, 0);

  LOG_Printf("\r\n========================================\r\n");
  LOG_Printf("\r\n\r\nSTM32F407 Top APP (DMA Full-Duplex)\r\n\r\n");
  LOG_Printf("========================================\r\n");
  /* Infinite loop */

  LOG_Printf("\r\nAsync Event Bus Ready.\r\n");
  vTaskDelete(NULL);
  /* USER CODE END StartStartupTask */
}

/* USER CODE BEGIN Header_StartShellTask */
/**
* @brief Function implementing the shellTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartShellTask */
void StartShellTask(void *argument)
{
  /* USER CODE BEGIN StartShellTask */
  ShellTaskFunction();
  /* Infinite loop */
  
  /* USER CODE END StartShellTask */
}

/* USER CODE BEGIN Header_StartLoggerTXTask */
/**
* @brief Function implementing the loggerTXTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartLoggerTXTask */
void StartLoggerTXTask(void *argument)
{
  /* USER CODE BEGIN StartLoggerTXTask */
  LoggerTXTaskFunction();
  /* Infinite loop */

  /* USER CODE END StartLoggerTXTask */
}

/* USER CODE BEGIN Header_StartEventBusTask */
/**
* @brief Function implementing the eventBusTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartEventBusTask */
void StartEventBusTask(void *argument)
{
  /* USER CODE BEGIN StartEventBusTask */
  EventBusTaskFunction();
  /* Infinite loop */

  /* USER CODE END StartEventBusTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

