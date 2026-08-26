/* ================================================================
 * ext_mem —— 外部 SRAM 统一内存池（first-fit + 边界标记 + canary）
 *
 * 架构位置：APP 服务层；LVGL 内存后端与通用大缓冲分配器。
 *
 * 设计要点（顶级可靠性/可观测性）：
 *   1. 边界标记（Knuth）：每块头尾各存 size|flags，释放时通过
 *      前/后邻居尾部标记 O(1) 双向合并，长期运行零碎片累积；
 *   2. canary 越界检测：头尾魔数 0xC0FFEE01，释放/巡检时校验，
 *      越界写在第一时间暴露并计数（canary_fail），不静默崩溃；
 *   3. 8 字节对齐：满足 LVGL 与 FPU/总线对齐访问要求；
 *   4. 线程安全：FreeRTOS 互斥量（EXT_MEM_NO_OS 编译宏去除，
 *      供主机单元测试）；
 *   5. 可观测：总量/已用/峰值/最大空闲块/分配失败/越界计数，
 *      GUI 面板与启动日志直读。
 *
 * 块布局（8 字节对齐，最小块 24B）：
 *   +0        size_and_flags（bit0=FREE）
 *   +4        magic 0xC0FFEE01
 *   +8        载荷（空闲块前 4 字节存 free-list next 池内偏移）
 *   +size-8   size_and_flags 副本（尾部边界标记）
 *   +size-4   magic（尾部 canary）
 * ================================================================ */
#include "ext_mem.h"

#include <string.h>

#ifdef EXT_MEM_NO_OS
#define EXT_MEM_LOCK()      do {} while (0)
#define EXT_MEM_UNLOCK()    do {} while (0)
#else
#include "FreeRTOS.h"
#include "semphr.h"
static StaticSemaphore_t s_lock_obj;
static SemaphoreHandle_t s_lock;
#define EXT_MEM_LOCK()      (void)xSemaphoreTake(s_lock, portMAX_DELAY)
#define EXT_MEM_UNLOCK()    (void)xSemaphoreGive(s_lock)
#endif

#define EXT_ALIGN           8u
#define EXT_ALIGN_UP(n)     (((uint32_t)(n) + (EXT_ALIGN - 1u)) & ~(EXT_ALIGN - 1u))

#define EXT_FLAG_FREE       0x01u
#define EXT_MAGIC           0xC0FFEE01u
#define EXT_HEAD_SIZE       8u      /* size_and_flags + magic */
#define EXT_TAIL_SIZE       8u      /* size_and_flags + magic */
#define EXT_OVERHEAD        (EXT_HEAD_SIZE + EXT_TAIL_SIZE)
#define EXT_MIN_BLOCK       24u     /* 最小块（空闲块还需 4B next 指针） */

typedef struct {
    uint32_t size;                 /* 块总字节数（含头尾），8 对齐；bit0=空闲标志 */
    uint32_t magic;                /* EXT_MAGIC */
} ext_blk_hdr_t;

typedef struct {
    uint32_t size;                 /* 与头部一致（含空闲标志） */
    uint32_t magic;                /* EXT_MAGIC（尾部 canary） */
} ext_blk_tlr_t;

typedef struct {
    uint8_t        *base;          /* 池基址 */
    uint32_t        total;         /* 池总字节数 */
    uint32_t        used;          /* 当前已用 */
    uint32_t        peak;          /* 峰值已用 */
    uint32_t        alloc_cnt;
    uint32_t        free_cnt;
    uint32_t        realloc_cnt;
    uint32_t        fail_cnt;
    uint32_t        canary_fail;
    ext_blk_hdr_t  *first;         /* free-list 头（池内块） */
    uint8_t         ready;
} ext_pool_t;

static ext_pool_t s_pool;

/* ---------------- 块元数据工具 ---------------- */

static uint32_t blk_size(const ext_blk_hdr_t *h)
{
    return h->size & ~EXT_FLAG_FREE;
}

static int blk_is_free(const ext_blk_hdr_t *h)
{
    return (int)(h->size & EXT_FLAG_FREE);
}

static ext_blk_tlr_t *blk_tail(ext_blk_hdr_t *h)
{
    return (ext_blk_tlr_t *)((uint8_t *)h + blk_size(h) - EXT_TAIL_SIZE);
}

/* 写块头尾（含空闲标志与魔数）——块布局唯一写入点 */
static void blk_write(ext_blk_hdr_t *h, uint32_t size, int is_free)
{
    h->size = size | (is_free ? EXT_FLAG_FREE : 0u);
    h->magic = EXT_MAGIC;
    ext_blk_tlr_t *t = blk_tail(h);
    t->size = h->size;
    t->magic = EXT_MAGIC;
}

/* 通过前一块的尾部边界标记定位前块（O(1)，无需遍历） */
static ext_blk_hdr_t *blk_prev(ext_blk_hdr_t *h)
{
    ext_blk_tlr_t *t = (ext_blk_tlr_t *)((uint8_t *)h - EXT_TAIL_SIZE);
    return (ext_blk_hdr_t *)((uint8_t *)t - (blk_size((ext_blk_hdr_t *)t) - EXT_TAIL_SIZE));
}

/* 池内有效块判定：地址在池内 + 对齐 + 头魔数（遍历防损坏崩溃） */
static int blk_valid(const ext_blk_hdr_t *h)
{
    const uint8_t *p = (const uint8_t *)h;
    if (p < s_pool.base || p >= (s_pool.base + s_pool.total)) {
        return 0;
    }
    if (((uintptr_t)h & (EXT_ALIGN - 1u)) != 0u) {
        return 0;
    }
    return h->magic == EXT_MAGIC;
}

/* ---------------- free-list（单链表，next 存池内偏移，32 位安全） ---------------- */

static uint32_t fl_next_off(ext_blk_hdr_t *h)
{
    return *(uint32_t *)((uint8_t *)h + EXT_HEAD_SIZE);
}

static void fl_set_next(ext_blk_hdr_t *h, uint32_t off)
{
    *(uint32_t *)((uint8_t *)h + EXT_HEAD_SIZE) = off;
}

static ext_blk_hdr_t *fl_next(ext_blk_hdr_t *h)
{
    uint32_t off = fl_next_off(h);
    return (off != 0u) ? (ext_blk_hdr_t *)(s_pool.base + off) : NULL;
}

static void fl_push(ext_blk_hdr_t *h)
{
    fl_set_next(h, (s_pool.first != NULL) ? (uint32_t)((uint8_t *)s_pool.first - s_pool.base) : 0u);
    s_pool.first = h;
}

static void fl_remove(ext_blk_hdr_t *h)
{
    ext_blk_hdr_t *cur = s_pool.first;
    ext_blk_hdr_t *prev = NULL;
    while (cur != NULL && cur != h) {
        prev = cur;
        cur = fl_next(cur);
    }
    if (cur == h) {
        uint32_t nxt = fl_next_off(h);
        if (prev != NULL) {
            fl_set_next(prev, nxt);
        } else {
            s_pool.first = (nxt != 0u) ? (ext_blk_hdr_t *)(s_pool.base + nxt) : NULL;
        }
    }
}

/* ---------------- 对外接口 ---------------- */

int ExtMem_Init(void *base, uint32_t size)
{
    if (base == NULL || size < (EXT_MIN_BLOCK * 4u)) {
        return -1;
    }
    if (((uintptr_t)base & (EXT_ALIGN - 1u)) != 0u) {
        return -1;
    }
    size &= ~(EXT_ALIGN - 1u);

    memset(&s_pool, 0, sizeof(s_pool));
#ifndef EXT_MEM_NO_OS
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_obj);
#endif
    s_pool.base  = (uint8_t *)base;
    s_pool.total = size;
    blk_write((ext_blk_hdr_t *)base, size, 1);   /* 整池一块空闲 */
    s_pool.first = (ext_blk_hdr_t *)base;
    s_pool.ready = 1u;
    return 0;
}

void *ExtMem_Alloc(uint32_t size)
{
    if (!s_pool.ready || size == 0u) {
        return NULL;
    }
    /* 溢出防护：size 上限先于对齐运算检查，避免 ALIGN_UP 回绕 */
    if (size > (s_pool.total - EXT_OVERHEAD)) {
        s_pool.fail_cnt++;
        return NULL;
    }
    uint32_t need = EXT_ALIGN_UP(size) + EXT_OVERHEAD;

    EXT_MEM_LOCK();

    /* first-fit：遍历 free-list，同时记住前驱以便摘除 */
    ext_blk_hdr_t *cur = s_pool.first;
    ext_blk_hdr_t *prev = NULL;
    while (cur != NULL && blk_size(cur) < need) {
        prev = cur;
        cur = fl_next(cur);
    }
    if (cur == NULL) {
        s_pool.fail_cnt++;
        EXT_MEM_UNLOCK();
        return NULL;
    }

    /* 摘除命中块 */
    uint32_t nxt = fl_next_off(cur);
    if (prev != NULL) {
        fl_set_next(prev, nxt);
    } else {
        s_pool.first = (nxt != 0u) ? (ext_blk_hdr_t *)(s_pool.base + nxt) : NULL;
    }

    /* 分裂：剩余空间足够一个最小块时切出，保持池连续铺满 */
    uint32_t fsz = blk_size(cur);
    if (fsz - need >= EXT_MIN_BLOCK) {
        ext_blk_hdr_t *rest = (ext_blk_hdr_t *)((uint8_t *)cur + need);
        blk_write(rest, fsz - need, 1);
        fl_push(rest);
        fsz = need;
    }
    blk_write(cur, fsz, 0);

    s_pool.used += fsz;
    if (s_pool.used > s_pool.peak) {
        s_pool.peak = s_pool.used;
    }
    s_pool.alloc_cnt++;

    EXT_MEM_UNLOCK();
    return (uint8_t *)cur + EXT_HEAD_SIZE;
}

void ExtMem_Free(void *ptr)
{
    if (ptr == NULL || !s_pool.ready) {
        return;
    }
    ext_blk_hdr_t *h = (ext_blk_hdr_t *)((uint8_t *)ptr - EXT_HEAD_SIZE);

    EXT_MEM_LOCK();

    /* 头部魔数校验（指针被破坏/非池指针） */
    if (h->magic != EXT_MAGIC) {
        s_pool.canary_fail++;
        EXT_MEM_UNLOCK();
        return;
    }
    /* 双重释放防护 */
    if (blk_is_free(h)) {
        s_pool.canary_fail++;
        EXT_MEM_UNLOCK();
        return;
    }
    /* 尾部 canary 校验：越界写在此暴露 */
    ext_blk_tlr_t *t = blk_tail(h);
    if (t->magic != EXT_MAGIC || t->size != h->size) {
        s_pool.canary_fail++;
    }

    s_pool.used -= blk_size(h);
    s_pool.free_cnt++;

    /* 后向合并：下一块空闲则吸收（从 free-list 摘除） */
    uint32_t sz = blk_size(h);
    uint32_t next_off = (uint32_t)((uint8_t *)h - s_pool.base) + sz;
    if (next_off < s_pool.total) {
        ext_blk_hdr_t *nh = (ext_blk_hdr_t *)(s_pool.base + next_off);
        if (blk_is_free(nh) && nh->magic == EXT_MAGIC) {
            fl_remove(nh);
            sz += blk_size(nh);
        }
    }
    /* 前向合并：前一块空闲则吸收（O(1) 边界标记） */
    if ((uint32_t)((uint8_t *)h - s_pool.base) >= EXT_TAIL_SIZE) {
        ext_blk_tlr_t *pt = (ext_blk_tlr_t *)((uint8_t *)h - EXT_TAIL_SIZE);
        if (pt->magic == EXT_MAGIC && (pt->size & EXT_FLAG_FREE) != 0u) {
            ext_blk_hdr_t *ph = blk_prev(h);
            uint32_t psz = blk_size(ph);
            fl_remove(ph);
            h = ph;
            sz += psz;
        }
    }
    blk_write(h, sz, 1);
    fl_push(h);

    EXT_MEM_UNLOCK();
}

void *ExtMem_Realloc(void *ptr, uint32_t size)
{
    if (ptr == NULL) {
        return ExtMem_Alloc(size);
    }
    if (size == 0u) {
        ExtMem_Free(ptr);
        return NULL;
    }
    ext_blk_hdr_t *h = (ext_blk_hdr_t *)((uint8_t *)ptr - EXT_HEAD_SIZE);
    if (!s_pool.ready || !blk_valid(h)) {
        s_pool.canary_fail++;           /* 非法指针防御：不 memcpy 越界 */
        return NULL;
    }
    uint32_t old_payload = blk_size(h) - EXT_OVERHEAD;
    if (size <= old_payload) {
        return ptr;                          /* 缩容：原地返回（不缩块，避免碎片抖动） */
    }
    void *np = ExtMem_Alloc(size);
    if (np == NULL) {
        return NULL;
    }
    memcpy(np, ptr, old_payload);
    ExtMem_Free(ptr);
    s_pool.realloc_cnt++;
    return np;
}

void ExtMem_GetStats(ext_mem_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    EXT_MEM_LOCK();
    uint32_t max_free = 0u;
    for (ext_blk_hdr_t *cur = s_pool.first; cur != NULL; cur = fl_next(cur)) {
        if (!blk_valid(cur)) {           /* 链损坏防御：停止遍历，不崩溃 */
            s_pool.canary_fail++;
            break;
        }
        uint32_t sz = blk_size(cur);
        if (sz > max_free) {
            max_free = sz;
        }
    }
    out->total       = s_pool.total;
    out->used        = s_pool.used;
    out->peak        = s_pool.peak;
    out->max_free    = max_free;
    out->alloc_cnt   = s_pool.alloc_cnt;
    out->free_cnt    = s_pool.free_cnt;
    out->realloc_cnt = s_pool.realloc_cnt;
    out->fail_cnt    = s_pool.fail_cnt;
    out->canary_fail = s_pool.canary_fail;
    EXT_MEM_UNLOCK();
}

int ExtMem_CheckIntegrity(void)
{
    int errs = 0;
    if (!s_pool.ready) {
        return 1;
    }
    EXT_MEM_LOCK();
    uint32_t off = 0u;
    while (off < s_pool.total) {
        ext_blk_hdr_t *h = (ext_blk_hdr_t *)(s_pool.base + off);
        uint32_t sz = blk_size(h);
        if (sz < EXT_MIN_BLOCK || (off + sz) > s_pool.total || h->magic != EXT_MAGIC) {
            errs++;
            break;
        }
        ext_blk_tlr_t *t = blk_tail(h);
        if (t->size != h->size || t->magic != EXT_MAGIC) {
            errs++;
        }
        off += sz;
    }
    EXT_MEM_UNLOCK();
    return errs;
}
