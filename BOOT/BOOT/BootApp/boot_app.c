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
#include "boot_param.h"

#include <stdio.h>
#include <string.h>

/* 断电注入测试：1=备份后 2=擦除后 3=写入后 4=提交后；0=禁用（生产必须为 0） */
#define POWERLOSS_TEST_STAGE  0

/* ---------- BOOT 升级状态广播（HOSTLINK 帧 0x0C，供上位机实时可视化） ---------- */
#define HOSTLINK_CMD_OTA_BOOT_STATUS  0x0C
#define BOOT_ST_PREP     1   /* 探测预下载包 */
#define BOOT_ST_VERIFY   2   /* 安全校验 */
#define BOOT_ST_BACKUP   3   /* 备份 RUN */
#define BOOT_ST_ERASE    4   /* 擦除 APP */
#define BOOT_ST_WRITE    5   /* 解密写入 */
#define BOOT_ST_COMMIT   6   /* 提交 PENDING */
#define BOOT_ST_DONE     7   /* 完成重启 */
#define BOOT_ST_FAIL     0xFF

static bool g_emit_status;   /* 单线程访问（BOOT 主流程），无需 volatile */

static uint16_t crc16_mbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* 通过 UART1 广播升级状态：AA 55 0C len(6) phase err version(4) crc16 */
static void boot_status_send(uint8_t phase, uint8_t err)
{
    if (!g_emit_status) return;
    uint8_t pkt[13];
    pkt[0] = 0xAA; pkt[1] = 0x55; pkt[2] = HOSTLINK_CMD_OTA_BOOT_STATUS;
    pkt[3] = 6; pkt[4] = 0;
    pkt[5] = phase;
    pkt[6] = err;
    uint32_t ver = *(volatile uint32_t *)APP_VERSION_ADDR;
    pkt[7]  = (uint8_t)(ver & 0xFF);
    pkt[8]  = (uint8_t)((ver >> 8) & 0xFF);
    pkt[9]  = (uint8_t)((ver >> 16) & 0xFF);
    pkt[10] = (uint8_t)((ver >> 24) & 0xFF);
    uint16_t crc = crc16_mbus(pkt, 11);
    pkt[11] = (uint8_t)(crc & 0xFF);
    pkt[12] = (uint8_t)((crc >> 8) & 0xFF);
    /* HOSTLINK 数据口为 921600：临时切换波特率发送状态帧，发完恢复 */
    uint32_t baud = huart1.Init.BaudRate;
    if (baud != 921600) {
        huart1.Init.BaudRate = 921600;
        HAL_UART_Init(&huart1);
    }
    HAL_UART_Transmit(&huart1, pkt, 13, 100);
    if (huart1.Init.BaudRate != baud) {
        huart1.Init.BaudRate = baud;
        HAL_UART_Init(&huart1);
    }
}

/* 备份域访问宏（HAL 第二参数为寄存器索引，BKP_DR1 = 索引 0） */
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
    IWDG->KR = 0xAAAA;   /* 无论如何先喂狗，保证诊断期间不超时 */
}

/* 日志串口（USART2, 115200-8-N-1）——printf 重定向见 usart.c */
static void boot_log_init(void)
{
    /* CubeMX 已初始化 USART2，fputc 重定向在 usart.c 实现 */
}

/* 检查固件有效性：魔数偏移按所在区（RUN/BACKUP）取；SP/PC 以 RUN 链接地址为准 */
static uint8_t boot_check_app_valid(uint32_t addr)
{
    uint32_t valid_off = (addr == BACKUP_BASE_ADDR) ? BACKUP_VALID_OFFSET
                                                    : APP_VALID_OFFSET;
    if (*(volatile uint32_t *)(addr + valid_off) != APP_VALID_MAGIC) {
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

    printf("[BOOT] Jump  : SP=0x%08X  PC=0x%08X  ver=%lu\r\n",
           app_stack, app_reset,
           (unsigned long)(*(volatile uint32_t *)APP_VERSION_ADDR));

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

static void boot_enter_upgrade_mode(void);

/* 用 BACKUP 区恢复 RUN（回滚/自动修复），成功返回 true */
static bool boot_restore_backup(void)
{
    uint32_t magic = *(volatile uint32_t *)(BACKUP_BASE_ADDR + BACKUP_VALID_OFFSET);
    uint32_t sp = *(volatile uint32_t *)BACKUP_BASE_ADDR;
    uint32_t pc = *(volatile uint32_t *)(BACKUP_BASE_ADDR + 4);
    if (!boot_check_app_valid(BACKUP_BASE_ADDR)) {
        printf("[RB] BACKUP invalid: magic=0x%08X sp=0x%08X pc=0x%08X\r\n",
               (unsigned)magic, (unsigned)sp, (unsigned)pc);
        return false;
    }
    printf("[RB] BACKUP valid, restoring RUN...\r\n");
    bool e = flash_erase(APP_BASE_ADDR, APP_BASE_ADDR + APP_SIZE - 1);
    bool c = flash_copy_raw(APP_BASE_ADDR, BACKUP_BASE_ADDR, BACKUP_SIZE);
    if (e && c) {
        /* RUN 尾部有效性由 BACKUP 尾 8 字节补齐（魔数 + 版本） */
        uint32_t mg  = *(volatile uint32_t *)BACKUP_VALID_ADDR;
        uint32_t ver = *(volatile uint32_t *)BACKUP_VERSION_ADDR;
        flash_write(APP_VALID_ADDR, (uint8_t *)&mg, sizeof(mg));
        flash_write(APP_VERSION_ADDR, (uint8_t *)&ver, sizeof(ver));
    }
    printf("[RB] erase=%d copy=%d\r\n", (int)e, (int)c);
    return e && c;
}

/* 回滚：BACKUP -> RUN，更新回滚计数，超限进入 RECOVERY */
static void boot_rollback(void)
{
    boot_param_t param;
    boot_param_load(&param);
    if (boot_restore_backup()) {
        param.boot_state = BOOT_STATE_NORMAL;
        param.boot_count = 0;
        param.rollback_count++;
        param.last_error = 0x1001;
        if (param.rollback_count >= MAX_ROLLBACK_COUNT) {
            param.boot_state = BOOT_STATE_RECOVERY;
            printf("Rollback limit reached, entering recovery mode.\r\n");
        }
        if (!boot_param_save(&param)) {
            printf("[BOOT] param save FAILED\r\n");
        }
        printf("[BOOT] Rollback OK, jumping to APP...\r\n");
        boot_jump_to_app(APP_BASE_ADDR);
        return;
    }
    printf("Rollback failed, entering upgrade mode.\r\n");
    boot_enter_upgrade_mode();
}

/* Apply the firmware package already staged in DOWNLOAD area:
 * verify -> backup RUN -> erase -> decrypt -> magic/version -> PENDING -> reboot.
 * Shared by runtime-OTA (pre-downloaded) and YMODEM receive paths.
 * Returns true on success (never returns, reboots); false on validation failure.
 * Flash-level failures halt (same safety semantics as before). */
static bool boot_apply_download(bool emit_status)
{
    g_emit_status = emit_status;
    boot_status_send(BOOT_ST_VERIFY, 0);

    boot_param_t param;
    boot_param_load(&param);   /* 读取 last_build_no 供防重放校验 */

    uint32_t current_version = 0;
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        current_version = *(volatile uint32_t *)APP_VERSION_ADDR;
    }
    printf("Current APP version: %lu, last build: %lu\r\n",
           (unsigned long)current_version, (unsigned long)param.last_build_no);

    uint32_t app_size = 0;
    int32_t sec = security_verify_and_decrypt(DOWNLOAD_BASE_ADDR, &app_size,
                                              current_version,
                                              param.last_build_no);
    if (sec != 0) {
        printf("Security verification failed! err=%ld\r\n", (long)sec);
        boot_status_send(BOOT_ST_FAIL, (uint8_t)(-sec));
        return false;
    }

    /* 升级前备份当前 RUN 到 BACKUP（若当前固件有效） */
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("Backing up current APP to BACKUP area...\r\n");
        boot_status_send(BOOT_ST_BACKUP, 0);
        if (!flash_erase(BACKUP_BASE_ADDR,
                         BACKUP_BASE_ADDR + BACKUP_SIZE - 1) ||
            !flash_copy_raw(BACKUP_BASE_ADDR, APP_BASE_ADDR, BACKUP_SIZE)) {
            printf("BACKUP failed! System halted.\r\n");
            while (1) { IWDG->KR = 0xAAAA; }
        }
        /* BACKUP 尾部补写有效性：RUN 尾 8 字节（魔数+版本）→ BACKUP 尾 8 字节 */
        uint32_t mg  = *(volatile uint32_t *)APP_VALID_ADDR;
        uint32_t ver = *(volatile uint32_t *)APP_VERSION_ADDR;
        flash_write(BACKUP_VALID_ADDR, (uint8_t *)&mg, sizeof(mg));
        flash_write(BACKUP_VERSION_ADDR, (uint8_t *)&ver, sizeof(ver));
    }
#if POWERLOSS_TEST_STAGE == 1
    boot_param_t plt; boot_param_load(&plt);
    if (plt.last_error != 0x5A5A) {
        plt.last_error = 0x5A5A;
        boot_param_save(&plt);
        printf("[PLT] power-loss after BACKUP\r\n");
        NVIC_SystemReset();
    }
#endif

    printf("Erasing APP area...\r\n");
    boot_status_send(BOOT_ST_ERASE, 0);
    if (!flash_erase(APP_BASE_ADDR, APP_BASE_ADDR + APP_SIZE - 1)) {
        printf("APP erase failed! System halted.\r\n");
        while (1) { IWDG->KR = 0xAAAA; }
    }
#if POWERLOSS_TEST_STAGE == 2
    boot_param_t plt; boot_param_load(&plt);
    if (plt.last_error != 0x5A5A) {
        plt.last_error = 0x5A5A;
        boot_param_save(&plt);
        printf("[PLT] power-loss after ERASE\r\n");
        NVIC_SystemReset();
    }
#endif

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
    boot_status_send(BOOT_ST_WRITE, 0);
#if POWERLOSS_TEST_STAGE == 3
    boot_param_t plt; boot_param_load(&plt);
    if (plt.last_error != 0x5A5A) {
        plt.last_error = 0x5A5A;
        boot_param_save(&plt);
        printf("[PLT] power-loss after WRITE\r\n");
        NVIC_SystemReset();
    }
#endif

    uint32_t sp = *(volatile uint32_t *)APP_BASE_ADDR;
    uint32_t pc = *(volatile uint32_t *)(APP_BASE_ADDR + 4);
    if (sp < 0x20000000 || sp > 0x20020000 ||
        pc < APP_BASE_ADDR || pc > APP_BASE_ADDR + APP_SIZE) {
        printf("APP vector invalid! SP=0x%08X PC=0x%08X\r\n", sp, pc);
        return false;
    }

    printf("Security verification passed. Writing magic and version...\r\n");
    boot_status_send(BOOT_ST_COMMIT, 0);
    uint32_t magic = APP_VALID_MAGIC;
    flash_write(APP_VALID_ADDR, (uint8_t *)&magic, sizeof(magic));
    flash_write(APP_VERSION_ADDR, (uint8_t *)&hdr.version, sizeof(hdr.version));

    /* 置"待确认"参数：新固件首次启动计数，供回滚状态机使用；
     * 同时记录构建号，构成防重放闭环。 */
    param.boot_state = BOOT_STATE_PENDING;
    param.boot_count = 1;
    param.last_error = 0;
    param.last_build_no = hdr.build_no;
    boot_param_save(&param);
#if POWERLOSS_TEST_STAGE == 4
    boot_param_t plt; boot_param_load(&plt);
    if (plt.last_error != 0x5A5A) {
        plt.last_error = 0x5A5A;
        boot_param_save(&plt);
        printf("[PLT] power-loss after COMMIT\r\n");
        NVIC_SystemReset();
    }
#endif

    BKP_WRITE(0, BOOT_FLAG_NONE);
    printf("Update successful! Rebooting to new APP...\r\n");
    boot_status_send(BOOT_ST_DONE, 0);
    HAL_Delay(100);
    NVIC_SystemReset();
    return true;   /* unreachable */
}

static void boot_enter_upgrade_mode(void)
{
    printf("[BOOT] Entering upgrade mode...\r\n");
    ymodem_port_init();

    uint32_t uid[3];
    uid[0] = *(volatile uint32_t *)0x1FFF7A10;
    uid[1] = *(volatile uint32_t *)0x1FFF7A14;
    uid[2] = *(volatile uint32_t *)0x1FFF7A18;
    printf("DEV_UID:%08X%08X%08X\r\n", uid[0], uid[1], uid[2]);
    char uid_str[32];
    sprintf(uid_str, "DEV_UID:%08X%08X%08X\r\n", uid[0], uid[1], uid[2]);
    HAL_UART_Transmit(&huart1, (uint8_t *)uid_str, strlen(uid_str), 1000);

    /* Runtime OTA: if the APP already staged a package in DOWNLOAD via HOSTLINK,
     * apply it directly (business-uninterrupted upgrade). Fall back to YMODEM only
     * when no valid pre-downloaded package exists. */
    ota_header_t probe;
    memcpy(&probe, (void *)DOWNLOAD_BASE_ADDR, sizeof(probe));
    if (probe.magic == 0x4F5441FE &&
        probe.firmware_size > 0 &&
        probe.firmware_size <= DOWNLOAD_SIZE - sizeof(ota_header_t) - OTA_SIGN_SIZE) {
        printf("Pre-downloaded package found, applying directly...\r\n");
        if (boot_apply_download(true)) {
            return;
        }
        /* 半成品/损坏/防重放拒绝：跳回 APP（支持断点续传或重新下载），
         * 而不是回退 YMODEM 把设备卡在升级模式。
         * 跳 APP 前归一化参数状态：此时已进入升级模式（延迟后擦除正常），
         * 避免启动早期擦参数扇区（已知会导致 Flash BSY 卡死）。 */
        printf("Pre-downloaded package invalid; booting APP for re-download...\r\n");
        if (boot_check_app_valid(APP_BASE_ADDR)) {
            boot_param_t np;
            boot_param_load(&np);
            np.boot_state = BOOT_STATE_NORMAL;
            np.boot_count = 0;
            boot_param_save(&np);
            boot_jump_to_app(APP_BASE_ADDR);
            return;
        }
    }

    /* 下载区只擦除一次：避免重试循环中反复擦 flash，阻塞 SWD 调试连接 */
    printf("Erasing Download area...\r\n");
    if (!flash_erase(DOWNLOAD_BASE_ADDR, DOWNLOAD_BASE_ADDR + DOWNLOAD_SIZE - 1)) {
        printf("Download erase failed! System halted.\r\n");
        while (1) { IWDG->KR = 0xAAAA; }
    }

    while (1) {
        ymodem_ctx_t ctx;
        ctx.flash_end = DOWNLOAD_BASE_ADDR + DOWNLOAD_SIZE;
        ymodem_status_t status = ymodem_receive(&ctx, DOWNLOAD_BASE_ADDR);
        if (status != YMODEM_OK) {
            printf("OTA receive failed, status: %d. Retrying...\r\n", status);
            IWDG->KR = 0xAAAA;
            continue;
        }
        printf("OTA success. File: %s, Size: %lu\r\n", ctx.file_name, ctx.received_size);

        if (!boot_apply_download(false)) {
            printf("Package invalid, waiting for valid firmware...\r\n");
            IWDG->KR = 0xAAAA;
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 启动横幅：ASCII logo + 平台信息（统一启动视觉风格） */
static void boot_print_banner(void)
{
    printf(
        "\r\n"
        "  _____  _____  _____ \r\n"
        " |  __ \\|  _  ||  _  |\r\n"
        " | |  \\/| | | || | | |\r\n"
        " | | __ | | | || | | |\r\n"
        " | |_\\ \\| |_| || |_| |\r\n"
        "  \\____/\\_____/\\_____|\r\n"
        "\r\n"
        "============================================================\r\n"
        "  D00 Embedded Platform | STM32F407 Industrial Bootloader\r\n"
        "  Secure A/B OTA | Startup Confirmed | Rollback Ready\r\n"
        "============================================================\r\n");
}

void BootApp_Run(void)
{
    boot_watchdog_start();
    boot_log_init();
    boot_print_banner();

    boot_param_t param;
    boot_param_load(&param);
    printf("[BOOT] State : %s (tries=%lu rollbacks=%lu crc=0x%08X)\r\n",
           (param.boot_state == BOOT_STATE_NORMAL) ? "NORMAL" :
           (param.boot_state == BOOT_STATE_UPGRADE) ? "UPGRADE" :
           (param.boot_state == BOOT_STATE_PENDING) ? "PENDING" :
           (param.boot_state == BOOT_STATE_RECOVERY) ? "RECOVERY" : "UNKNOWN",
           (unsigned long)param.boot_count,
           (unsigned long)param.rollback_count,
           (unsigned)param.crc32);

    /* 1) 强制升级标志（APP 主动触发） */
    if (BKP_READ(0) == BOOT_FLAG_UPGRADE) {
        printf("[BOOT] Upgrade flag set. Entering upgrade mode...\r\n");
        BKP_WRITE(0, BOOT_FLAG_NONE);
        boot_enter_upgrade_mode();
        return;
    }

    /* 2) 参数区升级请求（APP 运行时 OTA 完成下载后写入） */
    if (param.boot_state == BOOT_STATE_UPGRADE) {
        printf("Param upgrade request. Entering upgrade mode.\r\n");
        param.boot_state = BOOT_STATE_NORMAL;
        param.boot_count = 0;
        boot_param_save(&param);
        boot_enter_upgrade_mode();
        return;
    }

    /* 3) 待确认：新固件启动计数，超限回滚 */
    if (param.boot_state == BOOT_STATE_PENDING) {
        if (param.boot_count >= MAX_BOOT_TRIES) {
            printf("New APP failed %lu tries, rolling back...\r\n",
                   param.boot_count);
            boot_rollback();
            return;
        }
        param.boot_count++;
        boot_param_save(&param);
        if (boot_check_app_valid(APP_BASE_ADDR)) {
            printf("[BOOT] Pending boot #%lu, jumping to APP...\r\n",
                   param.boot_count);
            boot_jump_to_app(APP_BASE_ADDR);
            return;
        }
        printf("Pending APP invalid, rolling back...\r\n");
        boot_rollback();
        return;
    }

    /* 4) 恢复模式：回滚超限，等待上位机强制重刷 */
    if (param.boot_state == BOOT_STATE_RECOVERY) {
        printf("Recovery mode (rollbacks exceeded). Waiting for firmware...\r\n");
        boot_enter_upgrade_mode();
        return;
    }

    /* 5) 正常模式：RUN 有效则跳转；无效则 BACKUP 自动修复；再无则升级 */
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("[BOOT] APP   : valid, jumping to APP...\r\n");
        boot_jump_to_app(APP_BASE_ADDR);
        return;
    }
    if (boot_check_app_valid(BACKUP_BASE_ADDR)) {
        printf("[BOOT] APP invalid, restoring from BACKUP...\r\n");
        if (boot_restore_backup()) {
            printf("[BOOT] BACKUP restored, jumping to APP...\r\n");
            boot_jump_to_app(APP_BASE_ADDR);
            return;
        }
        printf("[BOOT] BACKUP restore failed!\r\n");
    }
    printf("[BOOT] No valid firmware, entering upgrade mode.\r\n");
    boot_enter_upgrade_mode();
}
