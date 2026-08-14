/* ================================================================
 * ext_store —— 外部 Flash 顶级存储服务层
 *
 * 架构位置：APP 服务层；业务（OTA/文件/日志/用户数据）唯一入口，
 *           下层只依赖 BSP 的 bsp_w25q128 硬件驱动，禁止业务直连。
 *
 * 能力矩阵（本层一次性解决，业务零负担）：
 *   - 分区表：16MB W25Q128 静态划分（4KB 扇区对齐，编译期常量）
 *   - 擦写策略：分区内边界校验、64KB 块擦除、坏区自动跳过
 *   - 坏区管理：擦写失败扇区位图标记（双份持久化），后续擦/写自动规避
 *   - 掉电安全：WriteSafe 双份交替写入 + CRC32 + 提交点，断电恒有一份有效
 *   - 磨损均衡：双份槽固定交替，天然均衡
 *
 * 设计原则：运行时零大缓冲（坏区表按需读、数据直通 BSP 内部 DMA 缓冲），
 *           接口按"分区 + 偏移"组织，业务不感知物理擦除/对齐/坏区。
 * ================================================================ */
#ifndef EXT_STORE_H
#define EXT_STORE_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 分区 ID ---------------- */
typedef enum {
    EXT_PART_OTA_DL = 0,   /* OTA 下载区：2MB（4×512KB 下载槽） */
    EXT_PART_IMG_LIB,      /* 固件镜像库：2MB（3×512KB 槽 + 索引） */
    EXT_PART_FS,           /* 文件系统区：8MB（LittleFS 预留） */
    EXT_PART_USER,         /* 用户数据区：3MB */
    EXT_PART_META,         /* 系统元数据：1MB（分区表镜像/坏区表/会话） */
    EXT_PART_COUNT
} ext_part_id_t;

/* ---------------- 分区描述符 ---------------- */
typedef struct {
    const char *name;       /* 分区名（shell/日志用） */
    uint32_t    base;       /* 外部 Flash 偏移 */
    uint32_t    size;       /* 大小（字节，4KB 对齐） */
    uint32_t    unit;       /* 推荐擦除单元（4KB） */
    uint32_t    flags;      /* 属性位 */
} ext_part_desc_t;

#define EXT_PART_F_WRITABLE  (1u << 0)   /* 可写分区 */
#define EXT_PART_F_BOOT_CRIT (1u << 1)   /* 启动关键（元数据/镜像库） */

/* ---------------- 返回码 ---------------- */
#define EXT_STORE_OK          0
#define EXT_STORE_ERR_PARAM  (-1)   /* 参数/越界 */
#define EXT_STORE_ERR_INIT   (-2)   /* 服务层未初始化 */
#define EXT_STORE_ERR_BUSY   (-3)   /* BSP 忙/并发冲突 */
#define EXT_STORE_ERR_RO     (-4)   /* 分区只读 */
#define EXT_STORE_ERR_BAD    (-5)   /* 目标区域含坏扇区 */
#define EXT_STORE_ERR_CRC    (-6)   /* 双份均 CRC 失败 */
#define EXT_STORE_ERR_IO     (-7)   /* 底层擦写失败 */

/* ---------------- 生命周期 ---------------- */
void ExtStore_Init(void);           /* probe + 分区表自检（模块化入口） */
int ExtStore_Probe(void);           /* 重探测（不改变状态） */

/* ---------------- 分区信息 ---------------- */
const ext_part_desc_t *ExtStore_GetPart(ext_part_id_t id);
uint32_t ExtStore_Size(ext_part_id_t id);

/* ---------------- 基础读写擦（业务直用） ---------------- */
int ExtStore_Erase(ext_part_id_t id);                 /* 整区擦（坏区跳过） */
int ExtStore_EraseRange(ext_part_id_t id, uint32_t off, uint32_t len);
int ExtStore_Read(ext_part_id_t id, uint32_t off, void *buf, uint32_t len);
int ExtStore_Write(ext_part_id_t id, uint32_t off, const void *data, uint32_t len);

/* ---------------- 掉电安全双份读写（元数据/关键数据） ---------------- */
/* 逻辑槽 slot 占物理 2×stride：两份分居 [2*slot*stride, +stride) 与
 * [2*slot*stride+stride, +stride)。写：先副后主（主为提交点）；
 * 读：主有效取主，否则取副。stride 须 ≥ len + 16（头+CRC）。 */
int ExtStore_WriteSafe(ext_part_id_t id, uint32_t slot, uint32_t stride,
                       const void *data, uint32_t len);
int ExtStore_ReadSafe(ext_part_id_t id, uint32_t slot, uint32_t stride,
                      void *buf, uint32_t len, uint32_t *out_ver);

/* ---------------- 坏区管理 ---------------- */
bool ExtStore_IsBad(ext_part_id_t id, uint32_t off);  /* off 所在 4KB 扇区 */
void ExtStore_MarkBad(ext_part_id_t id, uint32_t off);
void ExtStore_BadMapClear(void);    /* 清除坏区表（出厂/误标恢复） */

#endif /* EXT_STORE_H */
