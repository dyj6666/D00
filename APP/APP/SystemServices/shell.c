#include "shell.h"
#include "logger.h"
#include "app_config.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "cmsis_os.h"
#include "stream_buffer.h"
#include <stdarg.h>
#include <main.h>
#include "event_bus.h"

typedef void (*cmd_func_t)(const char *args);
typedef struct {
    const char *name;
    cmd_func_t  func;
} cmd_entry_t;

static void cmd_help(const char *args);
static void cmd_info(const char *args);
static void cmd_reset(const char *args);
static void cmd_led(const char *args);
static void cmd_taskstats(const char *args);
static void cmd_ota(const char *args);
static void cmd_sysmon(const char *args);
static void cmd_la_sram_speed(const char *args);
static void cmd_la_sram_full(const char *args);
static void cmd_la_sram_align(const char *args);
static void cmd_la_sram_rand(const char *args);


static const cmd_entry_t cmd_table[] = {
    {"help",  cmd_help},
    {"info",  cmd_info},
    {"reset", cmd_reset},
    {"led",   cmd_led},
    {"taskstats", cmd_taskstats},
    {"ota", cmd_ota},
    {"sysmon", cmd_sysmon},
    {"la_sram_full",  cmd_la_sram_full},
    {"la_sram_rand",  cmd_la_sram_rand},
    {"la_sram_align", cmd_la_sram_align},
    {"la_sram_speed", cmd_la_sram_speed},
};
#define CMD_COUNT (sizeof(cmd_table)/sizeof(cmd_table[0]))

static char cmd_line[SHELL_LINE_MAX];
static int  cmd_len = 0;

/* 执行当前命令 */
static void shell_execute(void)
{
    if (cmd_len == 0) return;

    char cmd[32], *args = NULL;
    int i = 0;
    while (i < cmd_len && !isspace((unsigned char)cmd_line[i]) && (i < sizeof(cmd)-1)) {
        cmd[i] = cmd_line[i];
        i++;
    }
    cmd[i] = '\0';
    if (i < cmd_len) {
        args = &cmd_line[i];
        while (*args && isspace((unsigned char)*args)) args++;
        if (*args == '\0') args = NULL;
    }

    const cmd_entry_t *p = NULL;
    for (size_t n = 0; n < CMD_COUNT; n++) {
        if (strcmp(cmd, cmd_table[n].name) == 0) {
            p = &cmd_table[n];
            break;
        }
    }
    if (p) {
        p->func(args);
    } else {
        LOG_Printf("Unknown command: %s\r\n", cmd);
    }
}

/* 处理每个接收字符 */
void Shell_ProcessChar(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        /* 执行命令：先换行，执行，然后空一行分隔下一次输入 */
        LOG_Printf("\r\n");            // 将光标移到新行（回显换行）
        shell_execute();              // 执行并输出结果
        LOG_Printf("\r\n");            // 命令后追加空行，为下一次输入留白
        cmd_len = 0;
        cmd_line[0] = '\0';
        return;
    }

    if (ch == 127 || ch == 8) {     /* 退格 */
        if (cmd_len > 0) {
            cmd_len--;
            LOG_Printf("\b \b");    /* 标准退格回显 */
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {    /* 可打印字符 */
        if (cmd_len < SHELL_LINE_MAX - 1) {
            cmd_line[cmd_len++] = (char)ch;
            cmd_line[cmd_len] = '\0';
            LOG_Printf("%c", ch);  /* 直接回显字符 */
        }
    }
}

void ShellTaskFunction(void)
{
    StreamBufferHandle_t rx = LOG_GetRxStream();
    for (;;) {
        uint8_t ch;
        if (xStreamBufferReceive(rx, &ch, 1, portMAX_DELAY) > 0) {
            Shell_ProcessChar(ch);
        }
    }
}

/* ---------- 命令实现 ---------- */
static void cmd_help(const char *args) {
    (void)args;
    LOG_Printf("Available commands:\r\n");
    for (size_t i = 0; i < CMD_COUNT; i++) {
        LOG_Printf("  %s\r\n", cmd_table[i].name);
    }
}

static void cmd_info(const char *args) {
    (void)args;
    LOG_Printf("STM32F407ZGT6 @ 168MHz\r\n");
    LOG_Printf("FreeRTOS %s\r\n", tskKERNEL_VERSION_NUMBER);
    LOG_Printf("Tasks: %ld\r\n", uxTaskGetNumberOfTasks());
}

static void cmd_reset(const char *args) {
    (void)args;
    LOG_Printf("Resetting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(10));
    NVIC_SystemReset();
}

static void cmd_led(const char *args) {

    if (args == NULL) {
        LOG_Printf("Usage: led on/off/toggle\r\n");
        return;
    }
    MSG_SEND_DATA(MODULE_SHELL, MSG_CMD_LED, args, strlen(args) + 1);
}

static void cmd_taskstats(const char *args) {
    (void)args;
    char stats_buf[512];
    vTaskList(stats_buf);
    LOG_Printf("Task\tState\tPrio\tStack\t#\r\n");
    LOG_Printf("%s\r\n", stats_buf);
    LOG_Printf("Free heap: %lu bytes\r\n", xPortGetFreeHeapSize());
}

static void cmd_ota(const char *args) {
    (void)args;
    LOG_Printf("OTA command received, publishing event...\r\n");
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_OTA_START);
}

static void cmd_sysmon(const char *args)
{
    (void)args;
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_SYSMON);
}

static void cmd_la_sram_full(const char *args)
{
    uint16_t *sram = (uint16_t *)0x68000000;
    uint32_t total = 512 * 1024;   // 1MB / 2 = 512K 个 16 位单元
    uint32_t errors = 0;
    uint32_t start = HAL_GetTick();
    
    LOG_Printf("Full SRAM test started...\r\n");
    
    for (uint32_t i = 0; i < total; i += 1024) {  // 每 1024 个单元采样测试，加速
        sram[i] = (uint16_t)(i & 0xFFFF);
    }
    
    for (uint32_t i = 0; i < total; i += 1024) {
        uint16_t val = sram[i];
        uint16_t expected = (uint16_t)(i & 0xFFFF);
        if (val != expected) {
            errors++;
            if (errors <= 5) {
                LOG_Printf("FAIL at 0x%08X: wrote 0x%04X, read 0x%04X\r\n",
                           (uint32_t)(0x68000000 + i * 2), expected, val);
            }
        }
    }
    
    uint32_t elapsed = HAL_GetTick() - start;
    LOG_Printf("Full SRAM test done: %lu errors in %lums\r\n", errors, elapsed);
}

#include <stdlib.h>

static void cmd_la_sram_rand(const char *args)
{
    uint16_t *sram = (uint16_t *)0x68000000;
    uint32_t iterations = 10000;
    uint32_t errors = 0;
    
    srand(HAL_GetTick());
    
    for (uint32_t i = 0; i < iterations; i++) {
        uint32_t addr = rand() % (512 * 1024);  // 随机 16 位单元索引
        uint16_t val = (uint16_t)(rand() & 0xFFFF);
        sram[addr] = val;
        if (sram[addr] != val) {
            errors++;
        }
    }
    
    LOG_Printf("Random SRAM test: %lu errors in %lu iterations\r\n", errors, iterations);
}

static void cmd_la_sram_align(const char *args)
{
    uint8_t *sram = (uint8_t *)0x68000000;
    uint32_t errors = 0;
    
    // 测试奇数地址写入
    sram[0x0001] = 0xA5;
    sram[0x0003] = 0x5A;
    if (sram[0x0001] != 0xA5 || sram[0x0003] != 0x5A) errors++;
    
    // 测试偶数地址写入
    sram[0x0000] = 0x12;
    sram[0x0002] = 0x34;
    if (sram[0x0000] != 0x12 || sram[0x0002] != 0x34) errors++;
    
    // 测试相邻地址互不干扰
    sram[0x1000] = 0xAA;
    sram[0x1001] = 0xBB;
    sram[0x1002] = 0xCC;
    if (sram[0x1000] != 0xAA || sram[0x1001] != 0xBB || sram[0x1002] != 0xCC) errors++;
    
    LOG_Printf("Alignment SRAM test: %lu errors\r\n", errors);
}

static void cmd_la_sram_speed(const char *args)
{
    uint16_t *sram = (uint16_t *)0x68000000;
    uint32_t count = 50000;
    uint32_t start = HAL_GetTick();
    
    for (uint32_t i = 0; i < count; i++) {
        sram[i] = (uint16_t)(i & 0xFFFF);
    }
    
    uint32_t elapsed = HAL_GetTick() - start;
    uint32_t speed = (count * 2) / elapsed;  // 字节/毫秒 = KB/s
    
    // 验证写入正确性
    uint32_t errors = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (sram[i] != (uint16_t)(i & 0xFFFF)) errors++;
    }
    
    LOG_Printf("Speed test: %lu writes in %lums (%lu KB/s), %lu errors\r\n",
               count, elapsed, speed, errors);
}