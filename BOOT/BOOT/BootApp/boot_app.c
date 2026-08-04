/* BOOT 应用层实现：从 CubeMX 生成的 main.c 中抽离的业务逻辑 */
#include "boot_app.h"
#include "boot_config.h"
#include "stm32f4xx_hal.h"
#include "iwdg.h"
#include "rtc.h"
#include "usart.h"
#include "ymodem.h"
#include "ymodem_port.h"
#include "flash_if.h"
#include "security.h"

#include <stdio.h>
#include <string.h>

/* 备份域访问宏，简化书写 */
#define BKP_READ(reg)   HAL_RTCEx_BKUPRead(&hrtc, (reg))
#define BKP_WRITE(reg, val) HAL_RTCEx_BKUPWrite(&hrtc, (reg), (val))

/* ------------------------------------------------------------------ */
/* 启动独立看门狗（一旦启动，无法软件关闭，直至复位） */
static void boot_watchdog_start(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER;
    hiwdg.Init.Reload = IWDG_RELOAD;
    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
        HAL_IWDG_Refresh(&hiwdg);
    }
}

/* 日志串口（USART2, 115200-8-N-1）——printf 重定向见 usart.c */
static void boot_log_init(void)
{
    /* CubeMX 已初始化 USART2，fputc 重定向在 usart.c 实现 */
}

/* 检查 APP 固件有效性 */
static uint8_t boot_check_app_valid(uint32_t addr)
{
    if (*(volatile uint32_t *)(addr + APP_VALID_OFFSET) != APP_VALID_MAGIC) {
        return 0;
    }
    uint32_t sp = *(volatile uint32_t *)addr;
    if (sp < 0x20000000 || sp > 0x20020000) {
        return 0;
    }
    uint32_t pc = *(volatile uint32_t *)(addr + 4);
    if (pc < APP_BASE_ADDR || pc > APP_BASE_ADDR + APP_SIZE) {
        return 0;
    }
    return 1;
}

/* 跳转到 APP：清外设/中断、设 VTOR、切栈、跳转 */
static void boot_jump_to_app(uint32_t addr)
{
    uint32_t app_stack = *(volatile uint32_t *)addr;
    uint32_t app_reset = *(volatile uint32_t *)(addr + 4);

    printf("Jumping to APP: SP=0x%08X, PC=0x%08X\r\n", app_stack, app_reset);

    HAL_UART_Abort(&huart2);
    HAL_UART_DeInit(&huart2);
    __HAL_RCC_USART2_FORCE_RESET();
    __HAL_RCC_USART2_RELEASE_RESET();

    HAL_UART_DeInit(&huart1);
    __HAL_RCC_USART1_FORCE_RESET();
    __HAL_RCC_USART1_RELEASE_RESET();
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    __HAL_RCC_DMA2_FORCE_RESET();
    __HAL_RCC_DMA2_RELEASE_RESET();

    IWDG->KR = 0xAAAA;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    SCB->VTOR = addr;
    __DSB();
    __ISB();

    __set_MSP(app_stack);
    __DSB();
    __ISB();

    ((void (*)(void))app_reset)();
}

/* 升级模式：接收 → 验证 → 解密写 APP → 写魔数 → 复位（防回滚） */
static void boot_enter_upgrade_mode(void)
{
    printf("Entering upgrade mode...\r\n");
    ymodem_port_init();

    uint32_t uid[3];
    uid[0] = *(volatile uint32_t *)0x1FFF7A10;
    uid[1] = *(volatile uint32_t *)0x1FFF7A14;
    uid[2] = *(volatile uint32_t *)0x1FFF7A18;
    printf("DEV_UID:%08X%08X%08X\r\n", uid[0], uid[1], uid[2]);
    char uid_str[32];
    sprintf(uid_str, "DEV_UID:%08X%08X%08X\r\n", uid[0], uid[1], uid[2]);
    HAL_UART_Transmit(&huart1, (uint8_t *)uid_str, strlen(uid_str), 1000);

    while (1) {
        printf("Erasing Download area...\r\n");
        if (!flash_erase(DOWNLOAD_BASE_ADDR, DOWNLOAD_BASE_ADDR + DOWNLOAD_SIZE - 1)) {
            printf("Download erase failed! System halted.\r\n");
            while (1) { IWDG->KR = 0xAAAA; }
        }

        ymodem_ctx_t ctx;
        ymodem_status_t status = ymodem_receive(&ctx, DOWNLOAD_BASE_ADDR);
        if (status != YMODEM_OK) {
            printf("OTA receive failed, status: %d. Retrying...\r\n", status);
            IWDG->KR = 0xAAAA;
            continue;
        }
        printf("OTA success. File: %s, Size: %lu\r\n", ctx.file_name, ctx.received_size);

        uint32_t current_version = 0;
        if (boot_check_app_valid(APP_BASE_ADDR)) {
            current_version = *(volatile uint32_t *)APP_VERSION_ADDR;
        }
        printf("Current APP version: %lu\r\n", current_version);

        uint32_t app_size = 0;
        if (!security_verify_and_decrypt(DOWNLOAD_BASE_ADDR, &app_size, current_version)) {
            printf("Security verification failed! Waiting for valid firmware...\r\n");
            IWDG->KR = 0xAAAA;
            continue;
        }

        printf("Erasing APP area...\r\n");
        if (!flash_erase(APP_BASE_ADDR, APP_BASE_ADDR + APP_SIZE - 1)) {
            printf("APP erase failed! System halted.\r\n");
            while (1) { IWDG->KR = 0xAAAA; }
        }

        ota_header_t hdr;
        memcpy(&hdr, (void *)DOWNLOAD_BASE_ADDR, sizeof(hdr));
        uint8_t iv16[16];
        memcpy(iv16, hdr.aes_iv, 12);
        memset(iv16 + 12, 0, 4);

        uint8_t aes_key[32];
        derive_aes_key(aes_key);        /* 设备 UID 派生 AES 密钥 */

        if (!aes_ctr_decrypt_to_flash(DOWNLOAD_BASE_ADDR + sizeof(ota_header_t),
                                      app_size, aes_key, iv16, APP_BASE_ADDR)) {
            printf("APP write failed! System halted.\r\n");
            while (1) { IWDG->KR = 0xAAAA; }
        }

        uint32_t sp = *(volatile uint32_t *)APP_BASE_ADDR;
        uint32_t pc = *(volatile uint32_t *)(APP_BASE_ADDR + 4);
        if (sp < 0x20000000 || sp > 0x20020000 ||
            pc < APP_BASE_ADDR || pc > APP_BASE_ADDR + APP_SIZE) {
            printf("APP vector invalid! SP=0x%08X PC=0x%08X\r\n", sp, pc);
            printf("Waiting for valid firmware...\r\n");
            IWDG->KR = 0xAAAA;
            continue;
        }

        printf("Security verification passed. Writing magic and version...\r\n");
        uint32_t magic = APP_VALID_MAGIC;
        flash_write(APP_VALID_ADDR, (uint8_t *)&magic, sizeof(magic));
        flash_write(APP_VERSION_ADDR, (uint8_t *)&hdr.version, sizeof(hdr.version));

        BKP_WRITE(RTC_BKP_DR1, BOOT_FLAG_NONE);
        printf("Update successful! Rebooting to new APP...\r\n");
        HAL_Delay(100);
        NVIC_SystemReset();
    }
}

/* ------------------------------------------------------------------ */
void BootApp_Run(void)
{
    boot_watchdog_start();
    boot_log_init();
    printf("BOOT Started.\r\n");

    if (BKP_READ(RTC_BKP_DR1) == BOOT_FLAG_UPGRADE) {
        printf("Upgrade flag set. Entering upgrade mode.\r\n");
        boot_enter_upgrade_mode();
    } else if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("APP valid, jumping to APP...\r\n");
        boot_jump_to_app(APP_BASE_ADDR);
    } else {
        printf("APP invalid, entering upgrade mode.\r\n");
        boot_enter_upgrade_mode();
    }
}
