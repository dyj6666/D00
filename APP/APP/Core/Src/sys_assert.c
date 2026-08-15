/* ================================================================
 * sys_assert —— ARMCC 5 标准库 assert 挂接实现
 *
 * AC5 的 <assert.h> 在未定义 NDEBUG 时引用 __aeabi_assert，
 * 第三方库（如 LVGL 内置 qrcodegen）未按 NDEBUG 裁剪时会触发。
 * 本实现将断言失败记录到硬件错误路径：写死循环（不喂狗，
 * 由 IWDG 复位兜底），避免未定义引用导致链接失败。
 * ================================================================ */
#include <stdint.h>

void __aeabi_assert(const char *expr, const char *file, int line)
{
    (void)expr;
    (void)file;
    (void)line;
    for (;;) { }
}
