#include "shell.h"
#include "bsp.h"
#include "logger.h"
#include "app_config.h"
#include "event_bus.h"
#include "la_sample.h"
#include "la_buffer.h"
#include "la_trigger.h"
#include "stream_buffer.h"
#include "task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void cmd_la_start(const char *args);
static void cmd_la_stop(const char *args);
static void cmd_la_trig(const char *args);
static void cmd_la_first(const char *args);
static void cmd_la_dma_start(const char *args);
static void cmd_la_dma_stop(const char *args);
static void cmd_la_read_pb4(const char *args);
static void cmd_la_peek(const char *args);

static const cmd_entry_t cmd_table[] = {
    {"help",         cmd_help},
    {"info",         cmd_info},
    {"reset",        cmd_reset},
    {"led",          cmd_led},
    {"taskstats",    cmd_taskstats},
    {"ota",          cmd_ota},
    {"sysmon",       cmd_sysmon},
    {"la_start",     cmd_la_start},
    {"la_stop",      cmd_la_stop},
    {"la_first",     cmd_la_first},
    {"la_trig",      cmd_la_trig},
    {"la_dma_start", cmd_la_dma_start},
    {"la_dma_stop",  cmd_la_dma_stop},
    {"la_read_pb4",  cmd_la_read_pb4},
    {"la_peek",      cmd_la_peek},
};
#define CMD_COUNT (sizeof(cmd_table) / sizeof(cmd_table[0]))

static char cmd_line[SHELL_LINE_MAX];
static int  cmd_len = 0;

/* ================== 命令执行 ================== */
static void shell_execute(void)
{
    if (cmd_len == 0) return;

    /* 拆分命令名与参数 */
    char cmd[32], *args = NULL;
    int i = 0;
    while (i < cmd_len && !isspace((unsigned char)cmd_line[i]) && (i < (int)sizeof(cmd) - 1)) {
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
        /* 回车执行：先换行，执行，再空一行分隔下一次输入 */
        LOG_Printf("\r\n");
        shell_execute();
        LOG_Printf("\r\n");
        cmd_len = 0;
        cmd_line[0] = '\0';
        return;
    }

    if (ch == 127 || ch == 8) {
        /* 退格 */
        if (cmd_len > 0) {
            cmd_len--;
            LOG_Printf("\b \b");
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {
        /* 可打印字符：回显并缓存 */
        if (cmd_len < SHELL_LINE_MAX - 1) {
            cmd_line[cmd_len++] = (char)ch;
            cmd_line[cmd_len] = '\0';
            LOG_Printf("%c", ch);
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

/* ================== 命令实现 ================== */
static void cmd_help(const char *args)
{
    (void)args;
    LOG_Printf("Available commands:\r\n");
    for (size_t i = 0; i < CMD_COUNT; i++) {
        LOG_Printf("  %s\r\n", cmd_table[i].name);
    }
}

static void cmd_info(const char *args)
{
    (void)args;
    LOG_Printf("STM32F407ZGT6 @ 168MHz\r\n");
    LOG_Printf("FreeRTOS %s\r\n", tskKERNEL_VERSION_NUMBER);
    LOG_Printf("Tasks: %ld\r\n", uxTaskGetNumberOfTasks());
}

static void cmd_reset(const char *args)
{
    (void)args;
    LOG_Printf("Resetting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(10));
    BSP_SystemReset();
}

static void cmd_led(const char *args)
{
    if (args == NULL) {
        LOG_Printf("Usage: led on/off/toggle\r\n");
        return;
    }
    MSG_SEND_DATA(MODULE_SHELL, MSG_CMD_LED, args, strlen(args) + 1);
}

static void cmd_taskstats(const char *args)
{
    (void)args;
    char stats_buf[512];
    vTaskList(stats_buf);
    LOG_Printf("Task\tState\tPrio\tStack\t#\r\n");
    LOG_Printf("%s\r\n", stats_buf);
    LOG_Printf("Free heap: %lu bytes\r\n", (unsigned long)xPortGetFreeHeapSize());
}

static void cmd_ota(const char *args)
{
    (void)args;
    LOG_Printf("OTA command received, publishing event...\r\n");
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_OTA_START);
}

static void cmd_sysmon(const char *args)
{
    (void)args;
    MSG_SEND_SIMPLE(MODULE_SHELL, MSG_CMD_SYSMON);
}

static void cmd_la_start(const char *args)
{
    (void)args;
    LA_Sample_Start(LA_MODE_TIMESTAMP);
    LOG_Printf("LA started\r\n");

    LA_Diag_PrintExtiStatus();
}

static void cmd_la_stop(const char *args)
{
    (void)args;
    LA_Sample_Stop();
    LOG_Printf("LA stopped, samples: %lu\r\n", LA_Buffer_GetCount());
}

static void cmd_la_first(const char *args)
{
    (void)args;
    if (LA_Buffer_GetCount() == 0) {
        LOG_Printf("No samples\r\n");
        return;
    }

    LA_SamplePoint pt;
    LA_Buffer_Read(&pt, 0, 1);
    uint32_t ts = ((uint32_t)pt.timestamp_hi << 16) | pt.timestamp_lo;
    LOG_Printf("First: ts=%lu, states=0x%02X\r\n", ts, pt.states);
}

static void cmd_la_trig(const char *args)
{
    /* 格式：la_trig <type> <channel>
       type: 0=off, 1=rising, 2=falling, 3=any */
    int type = 0, channel = 0;
    if (args) {
        sscanf(args, "%d %d", &type, &channel);
    }

    LA_Trigger_Set((LA_TriggerType)type, channel);
    if (type == LA_TRIG_NONE) {
        LOG_Printf("Trigger off\r\n");
    } else {
        LOG_Printf("Trigger: type=%d, ch=%d\r\n", type, channel);
    }
}

static void cmd_la_dma_start(const char *args)
{
    uint32_t rate = 100000;     /* 默认 100kHz */
    if (args) rate = atoi(args);
    LA_Sample_Start_DMA(rate);
}

static void cmd_la_dma_stop(const char *args)
{
    (void)args;
    uint32_t cnt = LA_Sample_Stop_DMA();
    LOG_Printf("DMA capture stopped, samples: %lu\r\n", cnt);

    if (cnt > 0) {
        uint32_t buf[4];
        LA_Sample_ReadDMABuffer(buf, 0, 4);
        LOG_Printf("First 4: %08lX %08lX %08lX %08lX\r\n",
                   buf[0], buf[1], buf[2], buf[3]);
    }
}

static void cmd_la_read_pb4(const char *args)
{
    (void)args;
    LOG_Printf("PB4 = %d\r\n", (LA_Sample_GetChannelStates() & 0x10) ? 1 : 0);
}

static void cmd_la_peek(const char *args)
{
    (void)args;
    LOG_Printf("PB4=%d, la_ch4=%lu, la_samples=%lu\r\n",
               (LA_Sample_GetChannelStates() & 0x10) ? 1 : 0, la_ch4_state, la_samples);
}
