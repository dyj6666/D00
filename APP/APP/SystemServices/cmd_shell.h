/* ================================================================
 * 命令核心（统一命令框架）
 *
 * 分层架构（上层一致，底层物理协议可插拔）：
 *
 *   cmd_catalog  命令目录：全部 cmd_* 实现，只声明 transport 掩码
 *        │  Cmd_Register / Cmd_DispatchLine
 *   cmd_shell   命令核心：注册表 / 会话上下文 / LOG_Printf 路由 /
 *               传输适配器注册表 / 流式会话助手
 *        │  cmd_transport_t（name / mask / start）
 *   ┌────┴────┬─────────┬──────────┐
 *   shell     tcp_svc  cmd_can    ...
 *   UART 适配  TCP 适配  CAN 适配（未来）
 *
 * 设计要点
 *   - 所有命令在 cmd_catalog 登记一次（Cmd_Register），help/补全/分发
 *     全部基于注册表，与具体传输无关；
 *   - 任一适配器把收到的行交给 Cmd_DispatchLine()，命令执行期间
 *     LOG_Printf 自动路由到该适配器输出，命令实现无需关心"我在哪个终端"；
 *   - 命令如需感知传输，调用 Cmd_ActiveTransport()/Cmd_ActiveUser()
 *     查询当前上下文（例如 net cap on 在 TCP 下自动取对端 IP）；
 *   - 多适配器并发分发由内部互斥量串行化，输出互不穿插。
 *
 * 新增物理协议（如 CAN）：实现一个 cmd_transport_t + 行来源/输出，
 *   调用 Cmd_TransportRegister() 注册一行即可；命令实现零改动。
 *   参考 SystemServices/cmd_can.c（模板）。
 * ================================================================ */
#ifndef CMD_SHELL_H
#define CMD_SHELL_H

#include <stdint.h>

/* 物理传输掩码：新增协议在适配器注册时扩展 */
#define CMD_TRANSPORT_UART   (1u << 0)   /* 调试串口 shell */
#define CMD_TRANSPORT_TCP    (1u << 1)   /* TCP 控制台 :9000 */
#define CMD_TRANSPORT_CAN    (1u << 2)   /* 预留：CAN shell 适配器 */
#define CMD_TRANSPORT_ALL    (CMD_TRANSPORT_UART | CMD_TRANSPORT_TCP | \
                              CMD_TRANSPORT_CAN)

/* ---------- 传输适配器（物理协议可插拔） ---------- */
typedef struct cmd_transport cmd_transport_t;
struct cmd_transport {
    const char *name;          /* "UART" / "TCP" / "CAN" */
    uint32_t    mask;          /* CMD_TRANSPORT_* 单 bit */
    void      (*start)(void);  /* 可选：创建接收任务/初始化硬件，可为 NULL */
};

void Cmd_TransportRegister(const cmd_transport_t *t);
uint32_t Cmd_TransportCount(void);
const cmd_transport_t *Cmd_TransportGet(uint32_t index);

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

/* ---------- 流式会话（行协议传输共用：TCP 现用，CAN 未来复用） ---------- */
#define CMD_LINE_MAX    160

typedef struct {
    cmd_ctx_t ctx;
    char      line[CMD_LINE_MAX];
    uint16_t  len;
} cmd_session_t;

void Cmd_SessionReset(cmd_session_t *s, uint32_t mask, void *user,
                      cmd_out_fn out);
/* 追加字节流：按行切分（CR/LF 结束）并分发，返回分发次数 */
uint32_t Cmd_SessionFeed(cmd_session_t *s, const uint8_t *data, uint16_t len);

/* 统一提示符（各终端一致） */
#define CMD_PROMPT  "D00> "

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

/* 传输掩码的显示名（注册表驱动，支持未来新增协议） */
const char *Cmd_TransportName(uint32_t mask);

#endif
