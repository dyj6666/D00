/* ================================================================
 * sys_assert —— ARMCC 5 标准库 assert 挂接实现
 *
 * AC5 的 <assert.h> 在未定义 NDEBUG 时引用 __aeabi_assert，
 * 第三方库（如 LVGL 内置 qrcodegen）未按 NDEBUG 裁剪时会触发。
 * 本实现将断言失败接入统一错误管理路径（与 configASSERT 一致）：
 * 记录文件/行号 → 崩溃诊断持久化 → 软复位，绝不死循环。
 * ================================================================ */
#include <stdint.h>
#include "err_mgr.h"

void __aeabi_assert(const char *expr, const char *file, int line)
{
    (void)expr;
    (void)file;
    /* 统一入口：诊断输出 + BKP 摘要 + 软复位自愈（configASSERT 同路径） */
    ERR_HandleAssert((uint32_t)line);
    for (;;) { }   /* 不会到达：ERR_HandleAssert 内部复位 */
}
