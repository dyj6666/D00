/* ================================================================
 * 统一命令框架实现
 * ================================================================ */
#include "cmd_shell.h"
#include "logger.h"

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static cmd_entry_t s_table[CMD_TABLE_MAX];
static uint32_t s_count = 0;
static osMutexId_t s_mutex = NULL;
static cmd_ctx_t *s_active = NULL;   /* 当前分发中的适配器上下文 */

static void cmd_log_sink(const char *s, uint16_t len);

void Cmd_Init(void)
{
    memset(s_table, 0, sizeof(s_table));
    s_count = 0;
    if (s_mutex == NULL) {
        s_mutex = osMutexNew(NULL);
    }
    /* LOG_Printf 输出路由：命令执行期间导向当前适配器 */
    LOG_SetSink(cmd_log_sink);
}

int Cmd_Register(const cmd_entry_t *table, uint32_t count)
{
    if (table == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (s_count >= CMD_TABLE_MAX) {
            return -2;                     /* 表满 */
        }
        for (uint32_t j = 0; j < s_count; j++) {
            if (strcmp(s_table[j].name, table[i].name) == 0) {
                return -3;                 /* 重名 */
            }
        }
        s_table[s_count++] = table[i];
    }
    return 0;
}

const cmd_entry_t *Cmd_Get(uint32_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    return &s_table[index];
}

uint32_t Cmd_Count(void)
{
    return s_count;
}

const char *Cmd_TransportName(uint32_t mask)
{
    if (mask == CMD_TRANSPORT_ALL) {
        return "ALL";
    }
    if (mask == CMD_TRANSPORT_UART) {
        return "UART";
    }
    if (mask == CMD_TRANSPORT_TCP) {
        return "TCP";
    }
    if (mask == CMD_TRANSPORT_CAN) {
        return "CAN";
    }
    return "?";
}

uint32_t Cmd_ActiveTransport(void)
{
    return (s_active != NULL) ? s_active->transport : 0;
}

void *Cmd_ActiveUser(void)
{
    return (s_active != NULL) ? s_active->user : NULL;
}

/* LOG_Printf 路由钩子：命令执行期间把系统打印导到当前适配器输出；
 * 非命令执行（空闲/中断）时退回原始串口输出。 */
static void cmd_log_sink(const char *s, uint16_t len)
{
    cmd_ctx_t *ctx = s_active;
    if (ctx != NULL && ctx->out != NULL) {
        ctx->out(ctx, s, len);
    } else {
        LOG_WriteRaw(s, len);
    }
}

void Cmd_DispatchLine(const char *line, cmd_ctx_t *ctx)
{
    char cmd[24];
    const char *args;
    size_t n;

    if (line == NULL || ctx == NULL) {
        return;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    n = 0;
    while (line[n] != '\0' && line[n] != ' ' && line[n] != '\t' &&
           n < sizeof(cmd) - 1) {
        cmd[n] = line[n];
        n++;
    }
    cmd[n] = '\0';
    args = line + n;
    while (*args == ' ' || *args == '\t') {
        args++;
    }
    if (*args == '\0') {
        args = NULL;
    }

    if (s_mutex != NULL && osMutexAcquire(s_mutex, osWaitForever) != osOK) {
        return;
    }
    cmd_ctx_t *prev = s_active;
    s_active = ctx;

    const cmd_entry_t *e = NULL;
    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_table[i].name, cmd) == 0) {
            e = &s_table[i];
            break;
        }
    }
    if (e == NULL) {
        LOG_Printf("Unknown command: %s (type 'help' for list)\r\n", cmd);
    } else if ((e->transport & ctx->transport) == 0) {
        LOG_Printf("%s: not available on %s\r\n", cmd,
                   Cmd_TransportName(ctx->transport));
    } else {
        e->func(args);
    }

    s_active = prev;
    if (s_mutex != NULL) {
        osMutexRelease(s_mutex);
    }
}

void Cmd_Help(cmd_ctx_t *ctx)
{
    (void)ctx;
    LOG_Printf("Available commands:\r\n");
    for (uint32_t i = 0; i < s_count; i++) {
        const cmd_entry_t *e = &s_table[i];
        if (e->transport == CMD_TRANSPORT_ALL) {
            LOG_Printf("  %-16s %s\r\n", e->name, e->brief);
        } else {
            LOG_Printf("  %-16s %s  [%s]\r\n", e->name, e->brief,
                       Cmd_TransportName(e->transport));
        }
    }
}
