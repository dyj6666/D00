/* ================================================================
 * GCC 工具链 newlib 系统调用桩（等价 CubeMX Core/Src/syscalls.c）
 * 仅供 CMake/GCC 构建使用；Keil 工程使用 ARMCC 自带运行库。
 * 工程统一走 LOG_Printf 输出，_write 置为丢弃（返回 len）。
 * ================================================================ */
#include <errno.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/unistd.h>
#include <stdint.h>

extern char end;      /* 由链接脚本 APP.ld 提供（.bss 末尾，堆起点） */
extern char _estack[]; /* 由链接脚本 APP.ld 提供（主栈顶，堆上限基准） */

int _close(int fd)
{
    (void)fd;
    return -1;
}

int _lseek(int fd, int ptr, int dir)
{
    (void)fd;
    (void)ptr;
    (void)dir;
    return 0;
}

int _read(int fd, char *ptr, int len)
{
    (void)fd;
    (void)ptr;
    (void)len;
    return 0;
}

int _write(int fd, const char *ptr, int len)
{
    (void)fd;
    (void)ptr;
    return len;              /* 丢弃：工程输出统一经 LOG_Printf */
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    if (st != NULL) {
        st->st_mode = S_IFCHR;
    }
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}

void *_sbrk(int incr)
{
    static char *heap_end = NULL;
    char *prev;

    if (heap_end == NULL) {
        heap_end = &end;
    }
    prev = heap_end;
    /* 堆上限用链接脚本的固定主栈顶（_estack），而非当前 SP：
     * FreeRTOS 任务运行在 PSP（任务栈在 CCM 0x1000xxxx），当前 SP 远低于
     * SRAM 堆区，若用 SP 做边界，malloc 恒判越界失败 → newlib rand() 的
     * "REENT malloc succeeded" assert 触发 → abort/_exit 死循环（实测挂死）。
     * 保留 1KB 主栈余量，与 APP.ld 的 _estack 定义一致。 */
    if (heap_end + incr > (char *)&_estack - 0x400) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return (void *)prev;
}

void _exit(int status)
{
    (void)status;
    for (;;) {
    }
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}

int _getpid(void)
{
    return 1;
}

clock_t _times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}
