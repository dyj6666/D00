#include "shell.h"
#include "bsp.h"
#include "logger.h"
#include "app_config.h"
#include "event_bus.h"
#include "la_sample.h"
#include "la_buffer.h"
#include "la_trigger.h"
#include "signal_gen.h"
#include "ota_agent.h"
#include "err_mgr.h"
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
static void cmd_la_dump(const char *args);
static void cmd_la_dma_stat(const char *args);
static void cmd_la_dma_buf(const char *args);
static void cmd_la_info(const char *args);
static void cmd_la_state(const char *args);
static void cmd_la_peek(const char *args);
static void cmd_sg_uart_start(const char *args);
static void cmd_sg_uart_stop(const char *args);
static void cmd_sg_uart_hex(const char *args);
static void cmd_sg_spi_start(const char *args);
static void cmd_sg_spi_stop(const char *args);
static void cmd_sg_i2c_start(const char *args);
static void cmd_sg_i2c_stop(const char *args);
static void cmd_sg_i2c_complex(const char *args);
static void cmd_ota_rbtest(const char *args);
static void cmd_eb_stress(const char *args);
#if CRASH_INJECT_ENABLE
static void cmd_crash(const char *args);
#endif

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
    {"la_dump",      cmd_la_dump},
    {"la_dma_stat",  cmd_la_dma_stat},
    {"la_dma_buf",   cmd_la_dma_buf},
    {"la_info",      cmd_la_info},
    {"la_state",     cmd_la_state},
    {"la_peek",      cmd_la_peek},
    {"sg_uart_start", cmd_sg_uart_start},
    {"sg_uart_stop",  cmd_sg_uart_stop},
    {"sg_uart_hex",   cmd_sg_uart_hex},
    {"sg_spi_start",  cmd_sg_spi_start},
    {"sg_spi_stop",   cmd_sg_spi_stop},
    {"sg_i2c_start",  cmd_sg_i2c_start},
    {"sg_i2c_stop",   cmd_sg_i2c_stop},
    {"sg_i2c_complex", cmd_sg_i2c_complex},
    {"ota_rbtest",     cmd_ota_rbtest},
    {"eb_stress",      cmd_eb_stress},
#if CRASH_INJECT_ENABLE
    {"crash",          cmd_crash},
#endif
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
    /* 格式：la_trig <type> <ch> [post] [cond_ch] [cond_level]
       type: 0=off 1=rising 2=falling 3=any
       post: 触发后采样点数（默认 2048）
       cond_ch/cond_level: 条件通道与电平（可选，如 I2C START：
       la_trig 2 0 2048 1 1 = CH0 下降沿且 CH1 为高时触发） */
    la_trigger_cfg_t cfg;
    LA_Trigger_GetConfig(&cfg);
    int type = 0, channel = 0, post = 0, cond_ch = -1, cond_level = 1;
    if (args) {
        int n = sscanf(args, "%d %d %d %d %d", &type, &channel, &post, &cond_ch, &cond_level);
        if (n >= 3 && post > 0) cfg.post_samples = (uint16_t)post;
        if (n >= 4 && cond_ch >= 0 && cond_ch < LA_MAX_CHANNELS) {
            cfg.cond_channel = (uint8_t)cond_ch;
            cfg.cond_level = (uint8_t)(cond_level != 0);
        }
    }
    cfg.type = (LA_TriggerType)type;
    cfg.channel = (uint8_t)channel;
    LA_Trigger_SetConfig(&cfg);

    if (cfg.type == LA_TRIG_NONE) {
        LOG_Printf("Trigger off\r\n");
    } else {
        LOG_Printf("Trigger: type=%d, ch=%d, post=%u",
                   cfg.type, cfg.channel, (unsigned)cfg.post_samples);
        if (cfg.cond_channel != 0xFF) {
            LOG_Printf(", cond=ch%d==%d", cfg.cond_channel, cfg.cond_level);
        }
        LOG_Printf("\r\n");
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

static void cmd_la_dump(const char *args)
{
    /* 格式：la_dump <count>（默认 512，上限为缓冲深度），导出 DMA 采样值 */
    uint32_t count = 512;
    if (args) count = (uint32_t)atoi(args);
    if (count == 0) count = 1;
    uint32_t cap = LA_Sample_GetDMABufferSize();
    if (count > cap) count = cap;
    if (count > 4096) {
        LOG_Printf("Dumping %lu samples, this will take a while...\r\n",
                   (unsigned long)count);
    }

    LOG_Printf("Dump %lu samples from DMA buffer:\r\n", (unsigned long)count);
    uint32_t buf[8];
    for (uint32_t i = 0; i < count; i += 8) {
        for (int k = 0; k < 8; k++) buf[k] = 0;
        uint32_t n = (count - i < 8) ? (count - i) : 8;
        LA_Sample_ReadDMABuffer(buf, i, n);
        LOG_Printf("%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
                   (unsigned long)buf[0], (unsigned long)buf[1],
                   (unsigned long)buf[2], (unsigned long)buf[3],
                   (unsigned long)buf[4], (unsigned long)buf[5],
                   (unsigned long)buf[6], (unsigned long)buf[7]);
        /* 限速：日志 TX 按 115200 波特率排空（约 11.5 KB/s），
         * 不延时会把 2 KB 流缓冲灌满并静默丢帧 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void cmd_la_dma_stat(const char *args)
{
    (void)args;
    LOG_Printf("DMA stat: count=%lu, buf=%lu pts, overrun=%u, src=%s\r\n",
               (unsigned long)LA_Sample_GetDMACount(),
               (unsigned long)LA_Sample_GetDMABufferSize(),
               LA_Sample_GetDMAOverrun(),
               LA_Sample_IsDMASram() ? "SRAM" : "IRAM");
}

static void cmd_la_dma_buf(const char *args)
{
    /* 格式：la_dma_buf <sram|iram> —— 切换 DMA 缓冲（SRAM 深 4 倍，IRAM 速率高） */
    if (args == NULL) {
        LOG_Printf("Usage: la_dma_buf <sram|iram>\r\n");
        return;
    }
    int use_sram = -1;
    if (strcmp(args, "sram") == 0) use_sram = 1;
    else if (strcmp(args, "iram") == 0) use_sram = 0;
    if (use_sram < 0) {
        LOG_Printf("Usage: la_dma_buf <sram|iram>\r\n");
        return;
    }
    if (LA_Sample_SetDMABuffer((uint8_t)use_sram) != 0) {
        LOG_Printf("SRAM 自检失败，无法切换\r\n");
    }
}

static void cmd_la_info(const char *args)
{
    la_trigger_cfg_t cfg;
    (void)args;
    LA_Trigger_GetConfig(&cfg);
    LOG_Printf("=== LA INFO ===\r\n");
    LOG_Printf("  DMA buffer: %lu pts (%s)\r\n",
               (unsigned long)LA_Sample_GetDMABufferSize(),
               LA_Sample_IsDMASram() ? "external SRAM" : "internal RAM");
    LOG_Printf("  SRAM self-test: %s\r\n", LA_Buffer_IsSramOk() ? "PASS" : "FAIL");
    LOG_Printf("  Trigger: type=%d ch=%d post=%u cond=",
               cfg.type, cfg.channel, (unsigned)cfg.post_samples);
    if (cfg.cond_channel == 0xFF) {
        LOG_Printf("none\r\n");
    } else {
        LOG_Printf("ch%d==%d\r\n", cfg.cond_channel, cfg.cond_level);
    }
}

static void cmd_la_state(const char *args)
{
    (void)args;
    uint8_t states = LA_Sample_GetChannelStates();
    LOG_Printf("CH states: 0x%02X (CH0=%d CH1=%d CH2=%d CH3=%d)\r\n",
               states, (states >> 0) & 1, (states >> 1) & 1,
               (states >> 2) & 1, (states >> 3) & 1);
}

static void cmd_la_peek(const char *args)
{
    (void)args;
    uint8_t states = LA_Sample_GetChannelStates();
    LOG_Printf("states=0x%02X, ch0=%d, ch3=%d, la_samples=%lu\r\n",
               states, (states & 0x01) ? 1 : 0, la_ch3_state, la_samples);
}

static void cmd_sg_uart_start(const char *args)
{
    uint32_t baud = 115200;
    char text[SG_TEXT_MAX] = "HELLO";
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%lu %63s %d", &baud, text, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_UartStart(baud, text, (uint16_t)interval);
    LOG_Printf("SG UART: %s (baud=%lu text=%s interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned long)baud, text, interval);
}

static void cmd_sg_uart_stop(const char *args)
{
    (void)args;
    SG_UartStop();
    LOG_Printf("SG UART: stopped\r\n");
}

static void cmd_sg_uart_hex(const char *args)
{
    uint32_t baud = 115200;
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%lu %127s %d", &baud, hex, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_UartStartHex(baud, hex, (uint16_t)interval);
    LOG_Printf("SG UART HEX: %s (baud=%lu len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned long)baud, strlen(hex) / 2, interval);
}

static void cmd_sg_spi_start(const char *args)
{
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%127s %d", hex, &interval);
    }
    if (interval < 1) interval = 1;
    int ret = SG_SpiStartHex(hex, (uint16_t)interval);
    LOG_Printf("SG SPI: %s (freq=164kHz mode0 len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED", strlen(hex) / 2, interval);
}

static void cmd_sg_spi_stop(const char *args)
{
    (void)args;
    SG_SpiStop();
    LOG_Printf("SG SPI: stopped\r\n");
}

static void cmd_sg_i2c_start(const char *args)
{
    unsigned addr = 0x50;
    char hex[SG_TEXT_MAX * 2 + 1] = {0};
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%x %127s %d", &addr, hex, &interval);
    }
    if (interval < 1) interval = 1;
    if (addr > 0x7F) addr = 0x50;
    int ret = SG_I2CStart((uint8_t)addr, hex, (uint16_t)interval);
    LOG_Printf("SG I2C: %s (addr=0x%02X len=%zu bytes interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED",
               (unsigned)addr, strlen(hex) / 2, interval);
}

static void cmd_sg_i2c_stop(const char *args)
{
    (void)args;
    SG_I2CStop();
    LOG_Printf("SG I2C: stopped\r\n");
}

static void cmd_sg_i2c_complex(const char *args)
{
    unsigned addr = 0x50;
    int interval = 5;
    if (args && *args) {
        sscanf(args, "%x %d", &addr, &interval);
    }
    if (interval < 1) interval = 1;
    if (addr > 0x7F) addr = 0x50;
    int ret = SG_I2CComplexStart((uint8_t)addr, (uint16_t)interval);
    LOG_Printf("SG I2C COMPLEX: %s (addr=0x%02X interval=%dms)\r\n",
               ret == 0 ? "STARTED" : "FAILED", (unsigned)addr, interval);
}

static void cmd_ota_rbtest(const char *args)
{
    (void)args;
    LOG_Printf("OTA rollback test: arming...\r\n");
    Ota_ForceRollbackTest();
}

#if CRASH_INJECT_ENABLE
/* ================== 崩溃注入（仅调试构建，用于验证纠错系统） ==================
 * 用法：
 *   crash bus     -> 写非法地址触发 BusFault
 *   crash undef   -> 跳转非法指令触发 UsageFault/HardFault
 *   crash stack   -> 无限递归触发 FreeRTOS 栈溢出检测
 *   crash assert  -> 直接调用 ERR_HandleAssert 模拟 RTOS 断言失败 */
__attribute__((noinline)) static void crash_bus(void)
{
    *(volatile uint32_t *)0xDEADBEEFu = 0x55u;
}

__attribute__((noinline)) static void crash_undef(void)
{
    ((void (*)(void))0xFFFFFFFFu)();
}

__attribute__((noinline)) static void crash_stack(int depth)
{
    volatile uint8_t pad[128];
    pad[0] = (uint8_t)0xAA;
    (void)pad;
    if (depth > 0) {
        taskYIELD();          /* 让调度器在递归间隙做栈溢出检查 */
        crash_stack(depth - 1);
    }
}

static void cmd_crash(const char *args)
{
    if (args == NULL) {
        LOG_Printf("Usage: crash <bus|undef|stack|assert>\r\n");
        return;
    }
    LOG_Printf("Injecting crash: %s\r\n", args);
    if (strcmp(args, "bus") == 0) {
        crash_bus();
    } else if (strcmp(args, "undef") == 0) {
        crash_undef();
    } else if (strcmp(args, "stack") == 0) {
        crash_stack(200);     /* 128B×200 远超 2KB 任务栈，触发溢出检测 */
    } else if (strcmp(args, "assert") == 0) {
        ERR_HandleAssert(0xBADFu);
    } else if (strcmp(args, "irq") == 0) {
        /* 使能并置位一个未实现处理器的中断，触发 Default_Handler 诊断 */
        NVIC_EnableIRQ(TIM4_IRQn);
        NVIC_SetPendingIRQ(TIM4_IRQn);
    } else if (strcmp(args, "unhandled") == 0) {
        /* 直接调用未处理中断诊断（绕过中断路径，用于定位） */
        ERR_HandleUnhandledIRQ(46u);
    } else {
        LOG_Printf("Unknown crash type\r\n");
    }
}
#endif

/* ================== 事件总线极限负载测试 ==================
 * 用法：
 *   eb_stress <count> [payload] [burst|steady]
 *   - burst ：挂起 eventBusTask 后连发，测纯发布速率与缓冲/池上限；
 *   - steady：不挂起连发，测系统稳态吞吐（消费者实时消化）与丢包拐点。
 * payload 为每条消息字节数（<= EVENT_BUS_MSG_MAX_PAYLOAD）。 */
extern TaskHandle_t eventBusTaskHandle;

static volatile uint32_t g_eb_processed = 0;
static uint8_t           g_eb_subscribed = 0;

static void eb_stress_handler(const message_t *msg)
{
    (void)msg;
    g_eb_processed++;
}

static void cmd_eb_stress(const char *args)
{
    uint32_t count = 10000;
    uint16_t payload = 0;
    char mode[16] = "burst";
    if (args && *args) {
        unsigned long c = 0;
        unsigned int p = 0;
        int n = sscanf(args, "%lu %u %15s", &c, &p, mode);
        if (n >= 1) count = (uint32_t)c;
        if (n >= 2) payload = (uint16_t)p;
    }
    if (count == 0) count = 1;
    if (payload > EVENT_BUS_MSG_MAX_PAYLOAD) payload = EVENT_BUS_MSG_MAX_PAYLOAD;
    int steady = (strcmp(mode, "steady") == 0);

    if (!g_eb_subscribed) {
        EventBus_Subscribe(MSG_EB_STRESS, eb_stress_handler);
        g_eb_subscribed = 1;
    }

    /* DWT 周期计数（168MHz） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t lost0 = EventBus_GetLostCount();
    uint32_t proc0 = g_eb_processed;

    if (!steady) {
        vTaskSuspend(eventBusTaskHandle);
    }
    DWT->CYCCNT = 0;
    uint32_t pub_ok = 0, pub_lost = 0;
    for (uint32_t i = 0; i < count; i++) {
        message_t *msg = NULL;
        if (EventBus_AllocMsg(MODULE_SHELL, MSG_EB_STRESS,
                              payload, &msg) == 0) {
            if (payload) memset(msg->payload, 0xA5, payload);
            if (EventBus_Publish(msg) == 0) {
                pub_ok++;
            } else {
                pub_lost++;
            }
        } else {
            pub_lost++;
        }
    }
    uint32_t pub_cycles = DWT->CYCCNT;
    if (!steady) {
        vTaskResume(eventBusTaskHandle);
    }

    /* 等待消费者消化完成（最多 5s） */
    uint32_t wait_ms = 0;
    while (g_eb_processed - proc0 < pub_ok && wait_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(2));
        wait_ms += 2;
    }
    uint32_t proc_done = g_eb_processed - proc0;
    uint32_t lost_delta = EventBus_GetLostCount() - lost0;

    /* 速率（整数计算，@168MHz） */
    uint32_t cpmsg = pub_cycles / count;              /* cycles/msg（含失败路径） */
    uint32_t pub_rate = cpmsg ? (168000000u / cpmsg) : 0;   /* 发布 msg/s */
    uint32_t total_us = pub_cycles / 168u + wait_ms * 1000u; /* 总耗时（µs） */
    uint64_t sys_rate64 = total_us ? ((uint64_t)proc_done * 1000000u / total_us) : 0;
    uint32_t sys_rate = (uint32_t)sys_rate64;

    LOG_Printf("EB STRESS: count=%lu payload=%u mode=%s\r\n",
               (unsigned long)count, (unsigned)payload, mode);
    LOG_Printf("  published OK: %lu, lost: %lu\r\n",
               (unsigned long)pub_ok, (unsigned long)pub_lost);
    LOG_Printf("  publish: %lu cycles (%lu cpmsg, %lu msg/s)\r\n",
               (unsigned long)pub_cycles, (unsigned long)cpmsg,
               (unsigned long)pub_rate);
    LOG_Printf("  processed: %lu / %lu after %lu ms\r\n",
               (unsigned long)proc_done, (unsigned long)pub_ok,
               (unsigned long)wait_ms);
    LOG_Printf("  system throughput: %lu msg/s (%lu us)\r\n",
               (unsigned long)sys_rate, (unsigned long)total_us);
    LOG_Printf("  total lost delta: %lu, pool free: %lu, queue: %lu\r\n",
               (unsigned long)lost_delta,
               (unsigned long)EventBus_GetPoolFreeCount(),
               (unsigned long)EventBus_GetQueueCount());
}
