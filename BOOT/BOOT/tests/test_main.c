/* ================================================================
 * test_main —— BOOT 主机测试统一入口（crc32/fifo + ymodem FSM）
 *
 * 各测试文件暴露 *_test_main()，本文件汇总退出码。
 * ================================================================ */
#include <stdio.h>

int services_test_main(void);
int ymodem_test_main(void);

int main(void)
{
    setbuf(stdout, NULL);   /* 崩溃时保证已打印内容可见 */
    int rc = 0;
    printf("============================================\n");
    printf("== BOOT host test suite ==\n");
    printf("============================================\n");
    rc |= services_test_main();
    rc |= ymodem_test_main();
    return rc ? 1 : 0;
}
