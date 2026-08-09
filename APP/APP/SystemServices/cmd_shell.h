/* ================================================================
 * 统一命令框架：一张命令表 + 多物理传输适配器
 *
 * 设计要点
 *   - 所有命令在注册表中登记一次（Cmd_Register），help/补全/分发
 *     全部基于注册表，与具体传输无关；
 *   - 任一适配器（UART / TCP / 未来 CAN...）把收到的行交给
 *     Cmd_DispatchLine()，命令执行期间 LOG_Printf 自动路由到该
 *     适配器的输出（cmd_sink），命令实现无需关心"我在哪个终端"；
 *   - 命令如需感知传输，调用 Cmd_ActiveTransport()/Cmd_ActiveUser()
 *     查询当前上下文（例如 net cap on 在 TCP 下自动取对端 IP）；
 *   - 多适配器并发分发由内部互斥量串行化，输出互不穿插。
 *
 * 扩展新物理传输（例如 CAN）的步骤
 *   1. 在 CMD_TRANSPORT_* 增加掩码位；
 *   2. 实现一个"行源 + 输出"适配器：
 *        - 从 CAN 帧组装命令行，调用 Cmd_DispatchLine(line, &ctx)；
 *        - ctx.out 负责把输出按 CAN 帧回传；
 *   3. 命令实现零改动。
 * ================================================================ */
#ifndef CMD_SHELL_H
#define CMD_SHELL_H

#include <stdint.h>

/* 物理传输掩码：新增协议在此扩展 */
#define CMD_TRANSPORT_UART   (1u << 0)   /* 调试串口 shell */
#define CMD_TRANSPORT_TCP    (1u << 1)   /* TCP 控制台 :9000 */
#define CMD_TRANSPORT_CAN    (1u << 2)   /* 预留：CAN shell 适配器 */
#define CMD_TRANSPORT_ALL    (CMD_TRANSPORT_UART | CMD_TRANSPORT_TCP | \
                              CMD_TRANSPORT_CAN)

typedef struct cmd_ctx cmd_ctx_t;

/* 适配器输出：把 len 字节原样送到当前传输 */
typedef void (*cmd_out_fn)(cmd_ctx_t *ctx, const char *s, uint16_t len);
/* 命令函数：args 为去除命令名后的参数字符串（可为 NULL） */
typedef void (*cmd_func_t)(const char *args);

struct cmd_ctx {
    uint32_t   transport;   /* 当前传输（单 bit 掩码） */
    void      *user;        /* 适配器私有数据（TCP: tcp_cli_t*，可为 NULL） */
    cmd_out_fn out;         /* 输出函数 */
};

typedef struct {
    const char *name;       /* 命令名 */
    const char *brief;      /* 帮助说明 */
    uint32_t    transport;  /* 允许执行的传输掩码（位或） */
    cmd_func_t  func;
} cmd_entry_t;

#define CMD_TABLE_MAX  48

/* 框架初始化（建互斥量、清空注册表）；模块注册表 prio=2 */
void Cmd_Init(void);

/* 注册命令表（Shell_Init 调用；重复注册同名命令返回 -1） */
int  Cmd_Register(const cmd_entry_t *table, uint32_t count);

/* 分发一行命令：解析命令名/参数，校验传输掩码后执行。
 * 执行期间 LOG_Printf 输出路由到 ctx（命令函数内可查当前传输）。 */
void Cmd_DispatchLine(const char *line, cmd_ctx_t *ctx);

/* 打印全部命令（help 命令使用） */
void Cmd_Help(cmd_ctx_t *ctx);

/* 注册表访问（Tab 补全等使用） */
const cmd_entry_t *Cmd_Get(uint32_t index);
uint32_t Cmd_Count(void);

/* 当前执行中的传输上下文（仅命令执行期间有效；空闲返回 0 / NULL） */
uint32_t Cmd_ActiveTransport(void);
void    *Cmd_ActiveUser(void);

/* 传输掩码的显示名："UART" / "TCP" / "CAN" / "ALL" */
const char *Cmd_TransportName(uint32_t mask);

#endif
