/* ================================================================
 * bsp_w25q128 -- 板载 W25Q128 SPI NOR Flash 驱动实现
 *
 * 传输策略：
 *   - 短命令（<16B）：寄存器级阻塞全双工，DWT 微秒超时
 *   - 数据段读：Fast Read(0x0B) + DMA2 双通道（RX=Stream0/TX=Stream3，
 *     CH3），512B dummy 缓冲循环复用，42MHz 满速零 CPU 干预
 *   - 数据段写：页编程(0x02) 阻塞满速，跨页自动拆分
 *   - 忙等待：轮询 SR1/WIP，HAL_Delay(2) 节流 + 定期喂狗
 *
 * 线程安全：公共 API 持全局忙锁（非阻塞），内部函数假定已持锁。
 * ================================================================ */
#include "bsp_w25q128.h"
#include "bsp_watchdog.h"
#include "logger.h"
#include "pinout.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---------------- W25Q128 命令码 ---------------- */
#define W25_CMD_WRITE_ENABLE   0x06u
#define W25_CMD_WRITE_DISABLE  0x04u
#define W25_CMD_READ_STATUS    0x05u
#define W25_CMD_READ_DATA      0x03u
#define W25_CMD_FAST_READ      0x0Bu
#define W25_CMD_PAGE_PROGRAM   0x02u
#define W25_CMD_SECTOR_ERASE   0x20u
#define W25_CMD_BLOCK_ERASE_32 0x52u
#define W25_CMD_BLOCK_ERASE_64 0xD8u
#define W25_CMD_CHIP_ERASE     0xC7u
#define W25_CMD_JEDEC_ID       0x9Fu

/* ---------------- 状态寄存器位 ---------------- */
#define W25_SR_BUSY            (1u << 0)   /* 忙标志：1=正在擦写 */
#define W25_SR_WEL             (1u << 1)   /* 写使能锁存 */

/* ---------------- 超时（ms） ---------------- */
#define W25_TMO_STATUS         50u
#define W25_TMO_PAGE_WRITE     200u
#define W25_TMO_SECTOR         2000u
#define W25_TMO_BLOCK32        5000u
#define W25_TMO_BLOCK64        8000u
#define W25_TMO_CHIP           300000u

/* DMA 分块：dummy 缓冲循环复用，块越小 RAM 越省，开销越大。
 * TX 需多发 1 个 dummy 时钟（覆盖 RX 末字节），故 dummy 留 8B 余量 */
#define W25_DMA_CHUNK          512u
#define W25_DMA_DUMMY          (W25_DMA_CHUNK + 8u)
/* SPI 字节级等待超时（µs）：正常传输 1 字节 ~0.24µs；
 * 系统负载（I2C/ETH 中断）下放宽到 5ms，避免写入误判超时 */
#define W25_SPI_BYTE_TMO_US    5000u

/* ---------------- 驱动状态 ---------------- */
static SPI_HandleTypeDef s_hspi;                 /* SPI1 句柄 */
static volatile uint8_t  s_busy;                 /* 长操作忙锁（非阻塞互斥） */
static uint8_t           s_ready;                /* Init 成功标志 */
static uint8_t           s_dummy[W25_DMA_DUMMY]; /* TX dummy 缓冲（全 0xFF） */
/* DMA RX 内部缓冲（SRAM，DMA 可访问）：
 * FreeRTOS heap 位于 CCM RAM(0x10000000)，任务栈/堆缓冲 DMA 无法访问
 * （总线错误 TEIF0）。所有读操作先 DMA 入本缓冲，再拷贝到调用方缓冲。 */
static uint8_t           s_dma_rx[W25_DMA_CHUNK] __attribute__((aligned(4)));
static bsp_w25q_info_t   s_info;                 /* 芯片信息缓存 */

/* ---------------- 片选 ---------------- */
#define W25_CS_LOW()  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET)
#define W25_CS_HIGH() HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET)

/* ---------------- 忙锁：非阻塞互斥，长操作期间保持 ---------------- */
static int w25_lock(void)
{
    __disable_irq();
    if (s_busy != 0u) {
        __enable_irq();
        return BSP_W25Q_ERR_BUSY;
    }
    s_busy = 1u;
    __enable_irq();
    return BSP_W25Q_OK;
}

static void w25_unlock(void)
{
    __disable_irq();
    s_busy = 0u;
    __enable_irq();
}

/* ---------------- DWT 微秒计数（初始化幂等） ---------------- */
static void w25_dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t w25_dwt_ticks(uint32_t us)
{
    /* F407 固定 168MHz：不依赖 SystemCoreClock（若未更新会导致超时误判） */
    return us * 168u;
}

/* ---------------- SPI 寄存器级传输（阻塞，DWT 超时） ---------------- */
static int w25_spi_wait(uint32_t mask, uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT;
    uint32_t cyc = w25_dwt_ticks(us);
    while ((SPI1->SR & mask) == 0u) {
        if ((DWT->CYCCNT - t0) > cyc) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
    }
    return BSP_W25Q_OK;
}

/* 等待标志位清零（BSY 等忙信号） */
static int w25_spi_wait_clear(uint32_t mask, uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT;
    uint32_t cyc = w25_dwt_ticks(us);
    while ((SPI1->SR & mask) != 0u) {
        if ((DWT->CYCCNT - t0) > cyc) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
    }
    return BSP_W25Q_OK;
}

/* 全双工逐字节传输：写 tx[i] 收 rx[i]（rx 可为 NULL 丢弃），防止 RXNE 溢出 */
static int w25_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    /* HAL_SPI_Init 不置位 SPE（传输函数才使能）；寄存器级传输自行开启 */
    SPI1->CR1 |= SPI_CR1_SPE;
    for (uint32_t i = 0u; i < len; i++) {
        if (w25_spi_wait(SPI_SR_TXE, W25_SPI_BYTE_TMO_US) != BSP_W25Q_OK) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
        SPI1->DR = (tx != NULL) ? tx[i] : 0xFFu;
        if (w25_spi_wait(SPI_SR_RXNE, W25_SPI_BYTE_TMO_US) != BSP_W25Q_OK) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
        uint8_t d = (uint8_t)SPI1->DR;
        if (rx != NULL) {
            rx[i] = d;
        }
    }
    if (w25_spi_wait_clear(SPI_SR_BSY, W25_SPI_BYTE_TMO_US * 4u) != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_TIMEOUT;
    }
    return BSP_W25Q_OK;
}

/* ---------------- DMA 数据段读取（Fast Read 后半段） ---------------- */
static int w25_spi_read_dma(uint8_t *rx, uint32_t len)
{
    /* 自管理 DMA（寄存器级）：不依赖 HAL SPI DMA 状态机。
     * TX=DMA2_Stream3_CH3（dummy 提供时钟），RX=DMA2_Stream0_CH3。 */
    const uint32_t cr_base = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_1 |  /* CH3 */
                             DMA_SxCR_MINC | DMA_SxCR_PL_1;         /* 递增+高优先级 */
    uint8_t *dst = s_dma_rx;         /* 无条件先入 SRAM 缓冲（CCM 不可 DMA） */

    /* 等待残留传输结束（EN 被软件清后需等 DMA 真正停止） */
    uint32_t tw = DWT->CYCCNT;
    while ((DMA2_Stream0->CR & DMA_SxCR_EN) != 0u ||
           (DMA2_Stream3->CR & DMA_SxCR_EN) != 0u) {
        if ((DWT->CYCCNT - tw) > w25_dwt_ticks(1000u)) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
    }

    DMA2_Stream3->CR = 0u;
    DMA2_Stream3->NDTR = len + 1u;   /* 多 1 dummy 时钟，确保 RX 收满末字节 */
    DMA2_Stream3->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream3->M0AR = (uint32_t)s_dummy;
    DMA2_Stream3->CR = cr_base | DMA_SxCR_DIR_0;   /* MEM->PERIPH */
    DMA2_Stream0->CR = 0u;
    DMA2_Stream0->NDTR = len;
    DMA2_Stream0->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream0->M0AR = (uint32_t)dst;
    DMA2_Stream0->CR = cr_base;                    /* PERIPH->MEM */

    /* 清 Stream0/3 全部标志（F407：Stream0-3 在 LIFCR 低寄存器组） */
    DMA2->LIFCR = DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 |
                  DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0 |
                  DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;

    DMA2_Stream3->CR |= DMA_SxCR_EN;
    DMA2_Stream0->CR |= DMA_SxCR_EN;
    SPI1->CR2 = SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN;

    /* 等待 RX DMA 完成（TCIF0），超时 = 传输时间 + 余量 */
    /* 超时下限 5ms：覆盖中断抢占等偶发延迟，正常块传输 ~104us */
    uint32_t tmo_us = ((len + 1u) * 8u / 42u + 100u) * 2u;
    if (tmo_us < 5000u) {
        tmo_us = 5000u;
    }
    uint32_t t0 = DWT->CYCCNT;
    while ((DMA2->LISR & DMA_LISR_TCIF0) == 0u) {
        if ((DWT->CYCCNT - t0) > w25_dwt_ticks(tmo_us)) {
            LOG_Printf("[W25Q] DMA tmo: LISR=0x%08X S0CR=0x%08X S0NDTR=%lu "
                       "S0M0AR=0x%08X S3CR=0x%08X S3NDTR=%lu S3M0AR=0x%08X "
                       "SPI CR1=0x%08X CR2=0x%02X SR=0x%02X\r\n",
                       (unsigned)DMA2->LISR, (unsigned)DMA2_Stream0->CR,
                       (unsigned long)DMA2_Stream0->NDTR,
                       (unsigned)DMA2_Stream0->M0AR,
                       (unsigned)DMA2_Stream3->CR,
                       (unsigned long)DMA2_Stream3->NDTR,
                       (unsigned)DMA2_Stream3->M0AR,
                       (unsigned)SPI1->CR1, (unsigned)(SPI1->CR2 & 0xFFu),
                       (unsigned)(SPI1->SR & 0xFFu));
            LOG_Printf("[W25Q] RCC: AHB1ENR=0x%08X (DMA2=%u) "
                       "APB2ENR=0x%08X (SPI1=%u)\r\n",
                       (unsigned)RCC->AHB1ENR,
                       (unsigned)((RCC->AHB1ENR >> 25) & 1u),
                       (unsigned)RCC->APB2ENR,
                       (unsigned)((RCC->APB2ENR >> 12) & 1u));
            DMA2_Stream0->CR &= ~DMA_SxCR_EN;
            DMA2_Stream3->CR &= ~DMA_SxCR_EN;
            SPI1->CR2 &= ~(SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
            return BSP_W25Q_ERR_TIMEOUT;
        }
    }
    /* 正常完成：停 DMA、清 SPI DMA 请求与标志 */
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    DMA2_Stream3->CR &= ~DMA_SxCR_EN;
    SPI1->CR2 &= ~(SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    DMA2->LIFCR = DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 |
                  DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0 |
                  DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;
    memcpy(rx, dst, len);            /* 拷贝回用户缓冲（支持 CCM/任意地址） */
    return BSP_W25Q_OK;
}

/* SPI1 全局中断：DMA 传输期间错误（OVR/MODF）处理 */
void SPI1_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&s_hspi);
}

/* DMA2 Stream0/3 中断保留为空（自管理 DMA 不用中断；防 Default_Handler 死循环） */
void DMA2_Stream0_IRQHandler(void) { }
void DMA2_Stream3_IRQHandler(void) { }

/* ---------------- 内部命令（假定已持锁） ---------------- */
static uint8_t w25_read_sr1(void)
{
    uint8_t cmd = W25_CMD_READ_STATUS;
    uint8_t sr = 0u;
    W25_CS_LOW();
    (void)w25_spi_xfer(&cmd, NULL, 1u);
    (void)w25_spi_xfer(NULL, &sr, 1u);
    W25_CS_HIGH();
    return sr;
}

static int w25_write_enable(void)
{
    uint8_t cmd = W25_CMD_WRITE_ENABLE;
    W25_CS_LOW();
    int rc = w25_spi_xfer(&cmd, NULL, 1u);
    W25_CS_HIGH();
    if (rc != BSP_W25Q_OK) {
        return rc;
    }
    return (w25_read_sr1() & W25_SR_WEL) ? BSP_W25Q_OK : BSP_W25Q_ERR_WEN;
}

static int w25_wait_busy(uint32_t tmo_ms)
{
    uint32_t elapsed = 0u;
    while (1u) {
        if ((w25_read_sr1() & W25_SR_BUSY) == 0u) {
            return BSP_W25Q_OK;
        }
        if (elapsed >= tmo_ms) {
            return BSP_W25Q_ERR_TIMEOUT;
        }
        HAL_Delay(2u);
        elapsed += 2u;
        if ((elapsed & 0x3Fu) == 0u) {   /* 每 ~128ms 喂狗，防长擦除复位 */
            BSP_Watchdog_Refresh();
        }
    }
}

/* ---------------- 公共 API ---------------- */

int BSP_W25Q128_Init(void)
{
    /* 1) JTAG 释放：F407 无 SYSCFG_CFGR（F1/F429 才有）；
     *    PB3/PB4 默认 AF0(SWJ)，HAL_SPI_Init 里配置为 AF5(SPI1) 后
     *    JTAG 功能自动让位，SW-DP(PA13/14) 不受影响，DAP 仍可用。 */

    /* 2) SPI1 主模式 8bit，CPOL=0/CPHA=0，BR=2 -> 42MHz（APB2=84MHz） */
    s_hspi.Instance = W25Q_SPI;
    s_hspi.Init.Mode = SPI_MODE_MASTER;
    s_hspi.Init.Direction = SPI_DIRECTION_2LINES;
    s_hspi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_hspi.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_hspi.Init.NSS = SPI_NSS_SOFT;
    s_hspi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    s_hspi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_hspi.Init.TIMode = SPI_TIMODE_DISABLE;
    s_hspi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    s_hspi.Init.CRCPolynomial = 10u;
    if (HAL_SPI_Init(&s_hspi) != HAL_OK) {
        return BSP_W25Q_ERR_SPI;
    }

    /* 3) DMA2 时钟（自管理 DMA 直接操作寄存器，无需 HAL 句柄/中断） */
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* 4) dummy 缓冲全 0xFF（TX 提供时钟） */
    memset(s_dummy, 0xFF, sizeof(s_dummy));

    /* 5) DWT 超时基准 */
    w25_dwt_enable();

    /* 6) 探测芯片 */
    int rc = BSP_W25Q128_Probe();
    if (rc == BSP_W25Q_OK) {
        s_ready = 1u;
    }
    return rc;
}

int BSP_W25Q128_Probe(void)
{
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    uint8_t cmd = W25_CMD_JEDEC_ID;
    uint8_t id[3] = {0u, 0u, 0u};
    int rc = BSP_W25Q_OK;
    W25_CS_LOW();
    rc = w25_spi_xfer(&cmd, NULL, 1u);   /* 命令字节 */
    if (rc == BSP_W25Q_OK) {
        rc = w25_spi_xfer(NULL, id, 3u); /* 3 字节 JEDEC ID */
    }
    W25_CS_HIGH();
    w25_unlock();
    if (rc != BSP_W25Q_OK) {
        return rc;
    }
    s_info.jedec[0] = id[0];
    s_info.jedec[1] = id[1];
    s_info.jedec[2] = id[2];
    s_info.size = BSP_W25Q_SIZE;
    s_info.page = BSP_W25Q_PAGE;
    s_info.sector = BSP_W25Q_SECTOR;
    /* 校验 W25Q128 JEDEC ID = EF 40 18 */
    if (id[0] == 0xEFu && id[1] == 0x40u && id[2] == 0x18u) {
        return BSP_W25Q_OK;
    }
    return BSP_W25Q_ERR_PROBE;
}

void BSP_W25Q128_Info(bsp_w25q_info_t *info)
{
    if (info != NULL) {
        *info = s_info;
    }
}

int BSP_W25Q128_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (buf == NULL || len == 0u) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (addr >= BSP_W25Q_SIZE || len > (BSP_W25Q_SIZE - addr)) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    /* Fast Read 命令头：0x0B + 24bit 地址 + 1 dummy */
    uint8_t hdr[5] = {
        W25_CMD_FAST_READ,
        (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
        0xFFu,
    };
    int rc = BSP_W25Q_OK;
    W25_CS_LOW();
    rc = w25_spi_xfer(hdr, NULL, sizeof(hdr));
    if (rc == BSP_W25Q_OK) {
        /* 数据段：DMA 分块循环，CS 全程保持低（连续读模式） */
        while (len > 0u && rc == BSP_W25Q_OK) {
            uint32_t chunk = (len > W25_DMA_CHUNK) ? W25_DMA_CHUNK : len;
            rc = w25_spi_read_dma(buf, chunk);
            buf += chunk;
            len -= chunk;
        }
    }
    W25_CS_HIGH();
    w25_unlock();
    return rc;
}

int BSP_W25Q128_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (buf == NULL || len == 0u) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (addr >= BSP_W25Q_SIZE || len > (BSP_W25Q_SIZE - addr)) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = BSP_W25Q_OK;
    while (len > 0u && rc == BSP_W25Q_OK) {
        uint32_t page_off = addr & (BSP_W25Q_PAGE - 1u);   /* 页内偏移 */
        uint32_t chunk = BSP_W25Q_PAGE - page_off;         /* 到页尾 */
        if (chunk > len) {
            chunk = len;
        }
        /* 页编程：写使能 -> 命令头+数据 -> 等待 WIP 清零 */
        rc = w25_write_enable();
        if (rc != BSP_W25Q_OK) {
            break;
        }
        uint8_t hdr[4] = {
            W25_CMD_PAGE_PROGRAM,
            (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
        };
        W25_CS_LOW();
        rc = w25_spi_xfer(hdr, NULL, sizeof(hdr));
        if (rc == BSP_W25Q_OK) {
            rc = w25_spi_xfer(buf, NULL, chunk);
        }
        W25_CS_HIGH();
        if (rc == BSP_W25Q_OK) {
            rc = w25_wait_busy(W25_TMO_PAGE_WRITE);
        }
        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    w25_unlock();
    return rc;
}

/* 通用擦除：写使能 -> 命令 -> 忙等待 */
static int w25_erase(uint8_t opcode, uint32_t addr, uint32_t tmo_ms)
{
    int rc = w25_write_enable();
    if (rc != BSP_W25Q_OK) {
        return rc;
    }
    uint8_t hdr[4] = { opcode,
                       (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                       (uint8_t)addr };
    W25_CS_LOW();
    rc = w25_spi_xfer(hdr, NULL, sizeof(hdr));
    W25_CS_HIGH();
    if (rc != BSP_W25Q_OK) {
        return rc;
    }
    return w25_wait_busy(tmo_ms);
}

int BSP_W25Q128_EraseSector(uint32_t addr)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (addr >= BSP_W25Q_SIZE || (addr & (BSP_W25Q_SECTOR - 1u)) != 0u) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = w25_erase(W25_CMD_SECTOR_ERASE, addr, W25_TMO_SECTOR);
    w25_unlock();
    return rc;
}

int BSP_W25Q128_EraseBlock32(uint32_t addr)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (addr >= BSP_W25Q_SIZE || (addr & 0x7FFFu) != 0u) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = w25_erase(W25_CMD_BLOCK_ERASE_32, addr, W25_TMO_BLOCK32);
    w25_unlock();
    return rc;
}

int BSP_W25Q128_EraseBlock64(uint32_t addr)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (addr >= BSP_W25Q_SIZE || (addr & 0xFFFFu) != 0u) {
        return BSP_W25Q_ERR_PARAM;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = w25_erase(W25_CMD_BLOCK_ERASE_64, addr, W25_TMO_BLOCK64);
    w25_unlock();
    return rc;
}

int BSP_W25Q128_EraseChip(void)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = w25_write_enable();
    if (rc == BSP_W25Q_OK) {
        uint8_t cmd = W25_CMD_CHIP_ERASE;
        W25_CS_LOW();
        rc = w25_spi_xfer(&cmd, NULL, 1u);
        W25_CS_HIGH();
    }
    if (rc == BSP_W25Q_OK) {
        rc = w25_wait_busy(W25_TMO_CHIP);
    }
    w25_unlock();
    return rc;
}

int BSP_W25Q128_WaitBusy(uint32_t timeout_ms)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    int rc = w25_wait_busy(timeout_ms != 0u ? timeout_ms : W25_TMO_STATUS);
    w25_unlock();
    return rc;
}

int BSP_W25Q128_Status(void)
{
    if (s_ready == 0u) {
        return BSP_W25Q_ERR_INIT;
    }
    if (w25_lock() != BSP_W25Q_OK) {
        return BSP_W25Q_ERR_BUSY;
    }
    uint8_t sr = w25_read_sr1();
    w25_unlock();
    return sr;
}

/* ---------------- DMA 中断（驱动自带，覆盖 startup 弱符号） ---------------- */
