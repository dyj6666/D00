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
#include "ota_source.h"
#include "esp_flash.h"
#include "ota_backup.h"

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

/* 板载有源蜂鸣器（PF8 高电平发声）：BOOT 升级关键阶段提示音。
 * 仅用 GPIO + HAL_Delay，不依赖任何外设库。 */
static void boot_buzzer_pulse(uint32_t ms)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOF_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Pin = GPIO_PIN_8;
    HAL_GPIO_Init(GPIOF, &gpio);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(ms);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_8, GPIO_PIN_RESET);
}

/* ---------- OTA 状态机提示音（节奏即进度，阻塞式，仅依赖 HAL_Delay） ----------
 * 设计：
 *   VERIFY/BACKUP/ERASE/WRITE —— 60ms 短音，工作节拍（滴）
 *   COMMIT                    —— 180ms 长音，提交完成即将重启（嘟）
 *   校验失败                  —— 三短（滴-滴-滴）
 *   回滚                      —— 两长（嘟-嘟）
 * APP 侧另有：开始（滴-滴-嘟）/下载完成（滴-滴）/启动确认（滴-滴-滴-嘟），
 * 前后呼应形成完整升级旋律。 */
static void boot_buzzer_play(const uint16_t *on_gap, uint8_t n)
{
    if (on_gap == NULL || n == 0) return;
    for (uint8_t i = 0; i < n; i++) {
        boot_buzzer_pulse((on_gap[i * 2u] > 0) ? on_gap[i * 2u] : 1u);
        if (i + 1u < n) {
            HAL_Delay(on_gap[i * 2u + 1u]);
        }
    }
}

static void boot_buzzer_stage(uint8_t phase)
{
    static const uint16_t tick[]   = { 60, 1 };              /* 工作节拍：滴 */
    static const uint16_t commit[] = { 180, 1 };             /* 提交：嘟 */
    static const uint16_t fail[]   = { 60, 60, 60, 60, 60, 1 }; /* 三短 */
    switch (phase) {
        case BOOT_ST_COMMIT: boot_buzzer_play(commit, 1); break;
        case BOOT_ST_FAIL:   boot_buzzer_play(fail, 3); break;
        default:             boot_buzzer_play(tick, 1); break;
    }
}

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

/* 校验 RUN 向量表（单一事实源，跳转前 / 升级写入后共用）：
 * SP 必须在 SRAM/CCM 内部（严格 < 末端+1，杜绝越界一格）；
 * PC 必须在 RUN 区间内 [APP_BASE, APP_BASE+APP_SIZE)。 */
static bool boot_vector_valid(uint32_t sp, uint32_t pc)
{
    bool sp_ok = (sp >= 0x20000000u && sp < 0x20020000u) ||
                 (sp >= 0x10000000u && sp < 0x10010000u);
    bool pc_ok = (pc >= APP_BASE_ADDR) && (pc < APP_BASE_ADDR + APP_SIZE);
    return sp_ok && pc_ok;
}

/* 检查 RUN 固件有效性：魔数 @ RUN 尾部；SP/PC 以 RUN 链接地址为准 */
static uint8_t boot_check_app_valid(uint32_t addr)
{
    if (*(volatile uint32_t *)(addr + APP_VALID_OFFSET) != APP_VALID_MAGIC) {
        return 0;
    }
    uint32_t sp = *(volatile uint32_t *)addr;
    /* F407 有效栈：SRAM 128KB（0x20000000~0x2001FFFF）或 CCM 64KB
     * （0x10000000~0x1000FFFF，主栈可放 CCM）。0x20020000+ 无映射。 */
    if (!boot_vector_valid(sp, *(volatile uint32_t *)(addr + 4))) {
        return 0;
    }
    return 1;
}

/* 裸跳板：设置 MSP 后立即跳转，中间无任何栈操作。
 * 不能写成普通 C 调用（编译器尾声可能在切栈后用新栈弹栈 → 越界 HardFault）。 */
#if defined(__GNUC__)
__attribute__((naked)) void boot_jump_exec(uint32_t sp, uint32_t pc)
{
    __asm volatile (
        "msr msp, r0\n"
        "dsb\n"
        "isb\n"
        "bx  r1\n"
    );
}
#else
__asm void boot_jump_exec(uint32_t sp, uint32_t pc)
{
    msr msp, r0
    dsb
    isb
    bx  r1
}
#endif

/* 跳转到 APP：清外设/中断、设 VTOR、切栈、跳转。
 * 切栈动作在裸跳板内完成，本函数尾声即使被编译器合并为尾调用，
 * 弹栈也发生在切栈之前（用 BOOT 自己的栈），从根上杜绝越界弹栈。 */
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

    __DSB();
    __ISB();
    boot_jump_exec(app_stack, app_reset);
}

static void boot_enter_upgrade_mode(void);

/* ---------------- YMODEM 兜底写目标：外部 ota_dl 槽（按需擦扇区） ---------------- */
static uint32_t s_ymodem_erased_sector = 0xFFFFFFFFu;

static bool ymodem_ext_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    if (off + len > ESP_OTA_DL_SIZE) {
        return false;
    }
    /* 按 4KB 扇区边界切分：单次写可能跨越扇区，而 NOR 页编程前必须
     * 已擦除目标扇区（0->1 位不可写）。不再依赖"帧长整除扇区大小"
     * 的隐式不变量，任何写入形态都安全。 */
    while (len > 0) {
        uint32_t sector = off / ESP_SECTOR_SIZE;
        uint32_t seg_len = len;
        if (sector != (off + len - 1) / ESP_SECTOR_SIZE) {
            seg_len = (sector + 1) * ESP_SECTOR_SIZE - off;   /* 本扇区剩余 */
        }
        if (sector != s_ymodem_erased_sector) {
            if (!EspFlash_EraseSector(ESP_OTA_BASE + sector * ESP_SECTOR_SIZE)) {
                printf("[YMODEM] ext erase failed @sector %lu\r\n",
                       (unsigned long)sector);
                return false;
            }
            s_ymodem_erased_sector = sector;
        }
        IWDG->KR = 0xAAAA;
        if (!EspFlash_Write(ESP_OTA_BASE + off, data, seg_len)) {
            return false;
        }
        off  += seg_len;
        data += seg_len;
        len  -= seg_len;
    }
    return true;
}

/* 从外部备份槽恢复 RUN（方案B：回滚源外移 img_lib 分区），成功返回 true */
static bool boot_restore_backup(void)
{
    if (!OtaBackup_IsValid()) {
        printf("[RB] External backup invalid\r\n");
        return false;
    }
    return OtaBackup_Restore();
}

/* 回滚：BACKUP -> RUN，更新回滚计数，超限进入 RECOVERY */
static void boot_rollback(void)
{
    boot_param_t param;
    boot_param_load(&param);
    static const uint16_t rollback_tone[] = { 160, 80, 160, 1 };
    boot_buzzer_play(rollback_tone, 2);   /* 回滚：嘟-嘟，提示降级事件 */
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

/* 升级流程 Flash 级失败的统一出口：归一参数 + 清升级标志 + 复位。
 * 不再永久 halt：复位后由启动状态机自愈——
 *   RUN 已被破坏 → "魔数无效 → 外部备份自动修复"；
 *   RUN 未动（如备份失败）→ 正常跳 APP，升级中止。
 * 参数归一可避免复位后再次进入升级模式造成擦写失败循环。 */
static void boot_abort_apply(const char *why, uint32_t err_code)
{
    boot_param_t p;
    boot_param_load(&p);
    p.boot_state = BOOT_STATE_NORMAL;
    p.last_error = err_code;
    (void)boot_param_save(&p);
    BKP_WRITE(0, BOOT_FLAG_NONE);
    printf("[BOOT] %s -> param normalized + reset (self-heal)\r\n", why);
    boot_status_send(BOOT_ST_FAIL, 0xEE);
    IWDG->KR = 0xAAAA;
    HAL_Delay(50);
    NVIC_SystemReset();
}

/* Apply the firmware package already staged in external ota_dl slot:
 * verify -> backup RUN(ext) -> erase -> decrypt -> magic/version -> PENDING -> reboot.
 * Shared by runtime-OTA (pre-downloaded) and YMODEM receive paths.
 * Returns true on success (never returns, reboots); false on validation failure.
 * Flash-level failures halt (same safety semantics as before). */
static bool boot_apply_download(bool emit_status)
{
    g_emit_status = emit_status;
    boot_status_send(BOOT_ST_VERIFY, 0);

    /* 读源：外部 ota_dl 槽为唯一来源（方案B，内部 DOWNLOAD 已取消） */
    ota_source_t src;
    bool ext_src = OtaBackup_Init() && OtaSource_External(&src);
    if (!ext_src) {
        printf("OTA source: no valid external package\r\n");
        return false;
    }
    printf("OTA source: EXTERNAL flash (pkg=%lu B)\r\n",
           (unsigned long)src.size);

    boot_param_t param;
    boot_param_load(&param);   /* 读取 last_build_no 供防重放校验 */

    uint32_t current_version = 0;
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        current_version = *(volatile uint32_t *)APP_VERSION_ADDR;
    }
    printf("Current APP version: %lu, last build: %lu\r\n",
           (unsigned long)current_version, (unsigned long)param.last_build_no);

    uint32_t app_size = 0;
    int32_t sec = security_verify_and_decrypt(&src, &app_size,
                                              current_version,
                                              param.last_build_no);
    if (sec != 0) {
        printf("Security verification failed! err=%ld\r\n", (long)sec);
        boot_status_send(BOOT_ST_FAIL, (uint8_t)(-sec));
        boot_buzzer_stage(BOOT_ST_FAIL);   /* 校验失败：三短警示 */
        return false;
    }
    boot_buzzer_stage(BOOT_ST_VERIFY);   /* 校验通过：一声短音 */

    /* 升级前备份当前 RUN 到外部 img_lib 槽（方案B；若当前固件有效） */
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("Backing up current APP to external flash...\r\n");
        boot_status_send(BOOT_ST_BACKUP, 0);
        if (!OtaBackup_Save()) {
            /* RUN 尚未被破坏：中止升级、归一参数复位回 APP 即可 */
            boot_abort_apply("BACKUP failed", 0x1002);
        }
        boot_buzzer_stage(BOOT_ST_BACKUP);   /* 备份完成：一声短音 */
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
        /* RUN 已破坏：复位后走"魔数无效 → 外部备份自动修复"自愈路径 */
        boot_abort_apply("APP erase failed", 0x1003);
    }
    boot_buzzer_stage(BOOT_ST_ERASE);   /* 擦除完成：一声短音 */
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
    src.read(0u, &hdr, sizeof(hdr));
    uint8_t iv16[16];
    memcpy(iv16, hdr.aes_iv, 12);
    memset(iv16 + 12, 0, 4);

    uint8_t aes_key[32];
    derive_aes_key(aes_key);        /* 设备 UID 派生 AES 密钥 */

    if (!aes_ctr_decrypt_to_flash(&src, sizeof(ota_header_t),
                                  app_size, aes_key, iv16, APP_BASE_ADDR)) {
        /* RUN 已损坏：复位后由外部备份自动修复 */
        boot_abort_apply("APP write failed", 0x1004);
    }
    boot_status_send(BOOT_ST_WRITE, 0);
    boot_buzzer_stage(BOOT_ST_WRITE);   /* 写入完成：一声短音 */
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
    /* 与 boot_check_app_valid 共用同一向量校验，边界严格一致 */
    if (!boot_vector_valid(sp, pc)) {
        printf("APP vector invalid! SP=0x%08X PC=0x%08X\r\n", sp, pc);
        boot_abort_apply("APP vector invalid", 0x1005);
    }

    printf("Security verification passed. Writing magic and version...\r\n");
    boot_status_send(BOOT_ST_COMMIT, 0);
    uint32_t magic = APP_VALID_MAGIC;
    flash_write(APP_VALID_ADDR, (uint8_t *)&magic, sizeof(magic));
    flash_write(APP_VERSION_ADDR, (uint8_t *)&hdr.version, sizeof(hdr.version));

    /* 置"待确认"参数：新固件首次启动计数，供回滚状态机使用；
     * 同时记录构建号，构成防重放闭环。保存失败 = 回滚保护/防重放
     * 静默丢失，必须重试并显式处置（不能无声继续）。 */
    param.boot_state = BOOT_STATE_PENDING;
    param.boot_count = 1;
    param.last_error = 0;
    param.last_build_no = hdr.build_no;
    if (!boot_param_save(&param) && !boot_param_save(&param)) {
        printf("[BOOT] PENDING param save FAILED (retried)! Halting.\r\n");
        boot_status_send(BOOT_ST_FAIL, 0xEF);
        while (1) { IWDG->KR = 0xAAAA; }   /* 宁停勿损：无 PENDING 保护不可上线 */
    }
    boot_buzzer_stage(BOOT_ST_COMMIT);  /* 提交完成：长音，即将重启切换 */
#if POWERLOSS_TEST_STAGE == 4
    boot_param_t plt; boot_param_load(&plt);
    if (plt.last_error != 0x5A5A) {
        plt.last_error = 0x5A5A;
        boot_param_save(&plt);
        printf("[PLT] power-loss after COMMIT\r\n");
        NVIC_SystemReset();
    }
#endif

    /* 升级成功：失效全部 DOWNLOAD 会话槽（魔数写 0，1→0 无需擦除）。
     * 防止同版本+同尺寸旧包被 APP 续传逻辑误判恢复（stale-resume）。 */
    for (uint32_t si = 0; si < BOOT_SESSION_SLOTS; si++) {
        uint32_t zero = 0;
        flash_write(BOOT_SESSION_BASE + si * BOOT_SESSION_STRIDE,
                    (uint8_t *)&zero, sizeof(zero));
    }
    /* 外部下载槽清理：升级成功后整体擦除（防重放）。外部备份槽保留至
     * 新固件启动确认（APP ota_confirm_startup 擦备份头）——PENDING 回滚
     * 期间必须持有旧固件快照，若此处清除将导致新固件崩溃时无源可回。 */
    (void)EspFlash_EraseRange64(ESP_OTA_BASE, ESP_OTA_DL_SIZE);
    printf("External OTA slot cleared (backup retained until confirm)\r\n");

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
    char uid_str[48];
    snprintf(uid_str, sizeof(uid_str), "DEV_UID:%08X%08X%08X\r\n",
             uid[0], uid[1], uid[2]);
    HAL_UART_Transmit(&huart1, (uint8_t *)uid_str, strlen(uid_str), 1000);

    /* Runtime OTA: if the APP already staged a package in external ota_dl slot
     * (via HOSTLINK/TCP/HTTP/CAN), apply it directly (business-uninterrupted
     * upgrade). Fall back to YMODEM only when no valid package exists. */
    ota_source_t src;
    if (OtaBackup_Init() && OtaSource_External(&src)) {
        printf("Pre-downloaded package found, applying directly...\r\n");
        if (boot_apply_download(true)) {
            return;
        }
    }

    /* 统一兜底（安全修复）：无论 probe 未命中、还是 apply 被安全校验拒绝
     * （坏密钥/防重放/损坏包），只要 RUN 固件有效就复位走正常启动路径回 APP，
     * 支持断点续传或重新下载；而不是落入 YMODEM 死等人工恢复。
     * 实测：坏密钥包（build 9031-9042）触发 apply 拒绝后，原实现落入
     * YMODEM 等待（COM13 'C' 洪流），需人工 YMODEM 才能恢复。
     * 采用"参数归一 + NVIC_SystemReset"而非直接跳转：走 BOOT 主流程的
     * 已验证跳转路径，且复位前参数已归一为 NORMAL，不会触发早期擦参数
     * 扇区（已知会导致 Flash BSY 卡死）。仅当 RUN 无效时才进 YMODEM。 */
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("No valid update; returning to APP for re-download...\r\n");
        boot_param_t np;
        boot_param_load(&np);
        np.boot_state = BOOT_STATE_NORMAL;
        np.boot_count = 0;
        boot_param_save(&np);
        BKP_WRITE(0, BOOT_FLAG_NONE);
        NVIC_SystemReset();
        return;   /* 不会到达 */
    }

    /* YMODEM 兜底：收包写入外部 ota_dl 槽（回调按需擦 4KB 扇区） */
    printf("YMODEM target: external ota_dl slot\r\n");
    s_ymodem_erased_sector = 0xFFFFFFFFu;
    while (1) {
        ymodem_ctx_t ctx;
        ctx.flash_end = ESP_OTA_DL_SIZE;
        ctx.write_fn  = ymodem_ext_write;
        ymodem_status_t status = ymodem_receive(&ctx, 0u);
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
    /* 统一初始化外部 Flash（方案B：备份/回滚/下载源均在外设）。
     * 必须早于 PENDING 回滚分支——否则 s_ready=false，OtaBackup_IsValid
     * 恒为 false，新固件启动失败时回滚源被误判为"无备份"而进入死循环。
     * （实测：回滚路径遗漏初始化 → 参数归一 NORMAL + 死循环固件反复重启） */
    (void)OtaBackup_Init();
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
        if (!boot_param_save(&param) && !boot_param_save(&param)) {
            /* 计数持久化失败：回滚阈值判定会延后，但不阻塞启动。
             * 显式告警而非静默继续。 */
            printf("[BOOT] WARN: PENDING count save failed\r\n");
        }
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

    /* 4) 恢复模式：回滚超限。设计意图是等待上位机强制重刷；
     * 但当前实现进升级模式后若无有效包仍会归一 NORMAL 跳 APP 跑旧固件
     * （见 boot_enter_upgrade_mode 兜底），RECOVERY 不阻塞运行——
     * 属有意妥协（宁可用旧固件也不砖机），升级通道随时可恢复。 */
    if (param.boot_state == BOOT_STATE_RECOVERY) {
        printf("Recovery mode (rollbacks exceeded). Waiting for firmware...\r\n");
        boot_enter_upgrade_mode();
        return;
    }

    /* 5) 正常模式：RUN 有效则跳转；无效则外部备份自动修复；再无则升级 */
    if (boot_check_app_valid(APP_BASE_ADDR)) {
        printf("[BOOT] APP   : valid, jumping to APP...\r\n");
        boot_jump_to_app(APP_BASE_ADDR);
        return;
    }
    if (OtaBackup_IsValid()) {
        printf("[BOOT] APP invalid, restoring from external backup...\r\n");
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
