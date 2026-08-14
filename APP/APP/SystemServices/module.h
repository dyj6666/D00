/* ================================================================
 * module —— 模块注册表接口：优先级排序 + 顺序初始化
 *
 * 架构位置：APP 服务层；启动流程唯一入口 modules_init()
 * ================================================================ */
#ifndef MODULE_H
#define MODULE_H

#include <stdint.h>

typedef void (*module_init_fn)(void);

typedef struct {
    const char    *name;      /* 模块名（启动日志用） */
    uint8_t        priority;  /* 初始化优先级：数值小者先执行 */
    module_init_fn init;
} module_desc_t;

/* 注册宏：显式给出名称与优先级 */
#define MODULE_INIT(name_str, prio, init_func) \
    { (name_str), (prio), (module_init_fn)(init_func) }

/* 兼容旧宏：以函数名作为模块名，默认优先级 100 */
#define REGISTER_MODULE(init_func) MODULE_INIT(#init_func, 100, (init_func))

void modules_init(void);

#endif
