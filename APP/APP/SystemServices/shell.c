/* ================================================================
 * UART Shell 传输适配器：串口命令终端
 *   - 上层命令语义在 cmd_catalog（传输无关）
 *   - 本文件只负责：RX 流 → 行编辑（历史/补全/方向键）→
 *     Cmd_DispatchLine（transport=UART）
 *   - 输出：命令执行期间 LOG_Printf 经命令核心路由到本适配器
 *   - 初始化时注册 "UART" 传输 + 命令目录
 * ================================================================ */
#include "shell.h"
#include "cmd_shell.h"
#include "cmd_catalog.h"
#include "logger.h"
#include "app_config.h"
#include "stream_buffer.h"
#include "task.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* UART 传输适配器描述：注册到命令核心（传输名/掩码） */
static const cmd_transport_t s_uart_transport = {
    .name  = "UART",
    .mask  = CMD_TRANSPORT_UART,
    .start = NULL,   /* 任务由 freertos.c 创建 */
};
static char cmd_line[SHELL_LINE_MAX];
static int  cmd_len = 0;

/* ---------- 历史记录（环形，上下键浏览） ---------- */
#define SHELL_HISTORY_MAX   8
static char shell_history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
static int  shell_hist_count = 0;
static int  shell_hist_pos = -1;      /* -1 = 正在编辑新行 */

/* ---------- ESC 序列状态机（方向键）---------- */
static int shell_esc_state = 0;       /* 0=普通 1=收到ESC 2=收到ESC[ */

static void shell_prompt(void)
{
    LOG_Printf(CMD_PROMPT);
}

/* 清空当前编辑行（\r + 空格覆盖 + \r，不依赖 ANSI，串口助手兼容） */
static void shell_clear_line(void)
{
    LOG_Printf("\r%-*s\r", SHELL_LINE_MAX, "");
}

static void shell_redraw(void)
{
    shell_clear_line();
    shell_prompt();
    if (cmd_len > 0) {
        LOG_Printf("%s", cmd_line);
    }
}

/* 当前行入历史（去重；满则丢弃最旧） */
static void shell_history_save(void)
{
    if (cmd_len == 0) return;
    if (shell_hist_count > 0 &&
        strcmp(shell_history[shell_hist_count - 1], cmd_line) == 0) {
        return;
    }
    if (shell_hist_count < SHELL_HISTORY_MAX) {
        strcpy(shell_history[shell_hist_count], cmd_line);
        shell_hist_count++;
    } else {
        memmove(shell_history[0], shell_history[1],
                (SHELL_HISTORY_MAX - 1) * SHELL_LINE_MAX);
        strcpy(shell_history[SHELL_HISTORY_MAX - 1], cmd_line);
    }
}

static void shell_history_nav(int dir)   /* -1 上一行 +1 下一行*/
{
    if (shell_hist_count == 0) return;
    if (dir < 0) {
        if (shell_hist_pos < 0) {
            shell_hist_pos = shell_hist_count - 1;
        } else if (shell_hist_pos > 0) {
            shell_hist_pos--;
        } else {
            return;
        }
    } else {
        if (shell_hist_pos < 0) return;
        shell_hist_pos++;
        if (shell_hist_pos >= shell_hist_count) {
            shell_hist_pos = -1;
            cmd_len = 0;
            cmd_line[0] = '\0';
            shell_redraw();
            return;
        }
    }
    strcpy(cmd_line, shell_history[shell_hist_pos]);
    cmd_len = (int)strlen(cmd_line);
    shell_redraw();
}

/* Tab 命令补全：唯一匹配补全，多匹配列出候选*/
static void shell_complete(void)
{
    int wlen = 0;
    while (wlen < cmd_len && !isspace((unsigned char)cmd_line[wlen])) {
        wlen++;
    }
    if (wlen == 0 || wlen < cmd_len) return;   /* 空行或已有参数不补全 */

    int match_count = 0;
    int match_idx = -1;
    for (uint32_t i = 0; i < Cmd_Count(); i++) {
        const cmd_entry_t *e = Cmd_Get(i);
        if (e != NULL && strncmp(e->name, cmd_line, (size_t)wlen) == 0) {
            match_count++;
            match_idx = (int)i;
        }
    }
    if (match_count == 1) {
        strcpy(cmd_line, Cmd_Get((uint32_t)match_idx)->name);
        cmd_len = (int)strlen(cmd_line);
        shell_redraw();
    } else if (match_count > 1) {
        LOG_Printf("\r\n");
        for (uint32_t i = 0; i < Cmd_Count(); i++) {
            const cmd_entry_t *e = Cmd_Get(i);
            if (e != NULL && strncmp(e->name, cmd_line, (size_t)wlen) == 0) {
                LOG_Printf("  %-16s %s\r\n", e->name, e->brief);
            }
        }
        shell_redraw();
    }
}

/* ================== 命令执行 ================== */
/* UART adapter output: route LOG_Printf to debug UART */
static void shell_uart_out(cmd_ctx_t *ctx, const char *s, uint16_t len)
{
    (void)ctx;
    LOG_WriteRaw(s, len);
}

/* Execute a line via the unified command framework (UART ctx) */
static void shell_execute(void)
{
    cmd_ctx_t ctx;
    if (cmd_len == 0) {
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.transport = CMD_TRANSPORT_UART;
    ctx.out = shell_uart_out;
    Cmd_DispatchLine(cmd_line, &ctx);
}


/* 处理每个接收字符 */
void Shell_ProcessChar(uint8_t ch)
{
    /* ESC 序列状态机（方向键）*/
    if (shell_esc_state == 1) {
        shell_esc_state = (ch == '[') ? 2 : 0;
        return;
    }
    if (shell_esc_state == 2) {
        if (ch == 'A') shell_history_nav(-1);   /* 上键 */
        else if (ch == 'B') shell_history_nav(1); /* 下键 */
        shell_esc_state = 0;
        return;
    }
    if (ch == 0x1B) {
        shell_esc_state = 1;
        return;
    }

    if (ch == '\r' || ch == '\n') {
        /* 回车执行：换行 → 存历史 → 执行 → 分隔行 → 提示符*/
        LOG_Printf("\r\n");
        shell_history_save();
        shell_execute();
        cmd_len = 0;
        cmd_line[0] = '\0';
        shell_hist_pos = -1;
        LOG_Printf("\r\n");
        shell_prompt();
        return;
    }

    if (ch == '\t') {
        shell_complete();
        return;
    }

    if (ch == 127 || ch == 8) {
        if (cmd_len > 0) {
            cmd_len--;
            cmd_line[cmd_len] = '\0';
            LOG_Printf("\b \b");
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {
        if (cmd_len < SHELL_LINE_MAX - 1) {
            cmd_line[cmd_len++] = (char)ch;
            cmd_line[cmd_len] = '\0';
            LOG_Printf("%c", ch);
        }
    }
}

/* Register the unified command table (shared by all adapters) */
void Shell_Init(void)
{
    Cmd_TransportRegister(&s_uart_transport);
    CmdCatalog_Register();
}

void ShellTaskFunction(void)
{
    StreamBufferHandle_t rx = LOG_GetRxStream();
    shell_prompt();   /* 初始提示符*/
    for (;;) {
        uint8_t ch;
        if (xStreamBufferReceive(rx, &ch, 1, portMAX_DELAY) > 0) {
            Shell_ProcessChar(ch);
        }
    }
}
