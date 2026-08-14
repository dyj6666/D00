/* ================================================================
 * bsp_w25q128 -- 板载 W25Q128 SPI NOR Flash 驱动（性能优先）
 *
 * 架构位置：APP BSP 层；上层（OTA 暂存/文件系统/用户数据）唯一
 *           允许访问外部 Flash 的接口，禁止直接调用 HAL_SPI。
 *
 * 硬件接线（探索者 V3 原理图，DAP 实测确认）：
 *   - SPI1 复用：PB3=SCK / PB4=MISO / PB5=MOSI（AF5）
 *   - 片选：PB14（F_CS），低电平有效
 *   - 注意：PB3/PB4 上电默认被 JTAG 占用，初始化时释放并切 SW-DP
 *
 * 性能设计（拉满到 F407 硬件极限）：
 *   - SPI1 挂在 APB2(84MHz)，BR=2 -> 42MHz（W25Q128 上限 104MHz）
 *   - 读：Fast Read(0x0B) + DMA2 双通道（RX=Stream0/TX=Stream3，
 *     通道 3），512B dummy 环形缓冲循环复用，CPU 零干预
 *   - 写：页编程(0x02) 256B 阻塞满速；跨页由 Write 自动拆分
 *   - 擦除：4KB 扇区(0x20) / 32KB 块(0x52) / 64KB 块(0xD8) / 整片
 *
 * 线程安全：无 OS 依赖；长操作持全局忙锁，并发调用立即返回
 *           BSP_W25Q_ERR_BUSY，调用方负责时序（如任务串行化）。
 * ================================================================ */
#ifndef BSP_W25Q128_H
#define BSP_W25Q128_H

#include <stdint.h>

/* ---------------- 容量 / 页 / 扇区 常量 ---------------- */
#define BSP_W25Q_SIZE        (16u * 1024u * 1024u)  /* 16MB */
#define BSP_W25Q_PAGE        256u                    /* 页编程大小 */
#define BSP_W25Q_SECTOR      4096u                   /* 最小擦除单元 */

/* ---------------- 返回码：0=成功，负=错误 ---------------- */
#define BSP_W25Q_OK          0
#define BSP_W25Q_ERR_PARAM  (-1)   /* 地址/长度越界或指针非法 */
#define BSP_W25Q_ERR_INIT   (-2)   /* 驱动未初始化 */
#define BSP_W25Q_ERR_BUSY   (-3)   /* 有长操作进行中（并发冲突） */
#define BSP_W25Q_ERR_TIMEOUT (-4)  /* 等待 WIP/SPI 超时 */
#define BSP_W25Q_ERR_WEN    (-5)   /* 写使能未生效 */
#define BSP_W25Q_ERR_SPI    (-6)   /* SPI 传输失败 */
#define BSP_W25Q_ERR_PROBE  (-7)   /* JEDEC ID 不匹配 */

/* ---------------- 芯片信息 ---------------- */
typedef struct {
    uint8_t  jedec[3];   /* JEDEC ID：0xEF 0x40 0x18 */
    uint32_t size;       /* 容量字节数 */
    uint32_t page;       /* 页大小 */
    uint32_t sector;     /* 扇区大小 */
} bsp_w25q_info_t;

/* 初始化：SPI1 + GPIO + JTAG 释放 + DMA 配置 + 探测；0=成功 */
int BSP_W25Q128_Init(void);

/* 重新探测（可随时调用，不改配置）；0=在线 */
int BSP_W25Q128_Probe(void);

/* 读取芯片信息（ID/容量/页/扇区） */
void BSP_W25Q128_Info(bsp_w25q_info_t *info);

/* 读任意长度（Fast Read + DMA，42MHz 满速） */
int BSP_W25Q128_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/* 写任意长度（自动 256B 页拆分 + 忙等待） */
int BSP_W25Q128_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

/* 擦除：扇区(4KB) / 块(32KB) / 块(64KB) / 整片 */
int BSP_W25Q128_EraseSector(uint32_t addr);
int BSP_W25Q128_EraseBlock32(uint32_t addr);
int BSP_W25Q128_EraseBlock64(uint32_t addr);
int BSP_W25Q128_EraseChip(void);

/* 等待芯片空闲（WIP 清零），timeout_ms=0 表示驱动默认超时 */
int BSP_W25Q128_WaitBusy(uint32_t timeout_ms);

/* 读状态寄存器 SR1；负=错误，否则返回 SR1 原值 */
int BSP_W25Q128_Status(void);

#endif /* BSP_W25Q128_H */
