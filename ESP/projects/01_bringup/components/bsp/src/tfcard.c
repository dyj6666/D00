/* ================================================================
 * tfcard.c —— TF 卡（SDSPI + FATFS）
 *
 * 实现路线（两段，第二段是本文件末尾的自研块设备）：
 *   1. 裸 SPI 探测 bsp_tfcard_probe_raw()：用普通 GPIO 手敲 74 时钟 + CMD0 + CMD8，
 *      用于把"卡的问题"和"驱动/配置的问题"分开（也是屏幕「SD探测」行的数据来源）；
 *   2. 自研 SD-SPI 块设备：卡初始化 + CMD17/CMD24 读写 + FATFS diskio 接口。
 *      **不再使用 IDF 的 SDSPI**——本板上它收不到卡的应答（见文件末尾说明）。
 * ================================================================ */
#include "bsp/tfcard.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/spi_bus.h"

#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "diskio.h"
#include "diskio_impl.h"
#include "ff.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "BSP/TF";

#define TF_MAX_FILES            8U
#define TF_ALLOC_UNIT           16384U

static bool          s_format_if_failed = false;
static esp_err_t     s_last_err = ESP_OK;

/* 自研块设备的状态 */
static FATFS        *s_fs = NULL;
static BYTE          s_pdrv = 0xFF;
static char          s_drive[3] = { '0', ':', '\0' };
static bool          s_sd_ready = false;
static bool          s_sd_sdhc = false;
static uint32_t      s_sd_sectors = 0;

/* 前置声明（init 在前面，实现放在文件后半段的自研块设备部分） */
static esp_err_t sd_card_mount(void);
static void      sd_card_unmount(void);
static esp_err_t sd_card_format(void);

void bsp_tfcard_format_if_failed(bool enable)
{
    s_format_if_failed = enable;
}

esp_err_t bsp_tfcard_init(void)
{
    if (s_sd_ready) {
        return ESP_OK;                          /* 幂等 */
    }

    /* 走自研块设备挂载（不用 IDF 的 SDSPI，原因见文件末尾的说明与 ENGINEERING_LOG 第 5 条） */
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        s_last_err = err;
        return err;
    }

    s_last_err = ESP_OK;
    return ESP_OK;
}

esp_err_t bsp_tfcard_deinit(void)
{
    if (!s_sd_ready) {
        return ESP_OK;
    }
    sd_card_unmount();
    return ESP_OK;
}

esp_err_t bsp_tfcard_remount(void)
{
    (void)bsp_tfcard_deinit();      /* 已挂载则先卸载；未挂载时是空操作 */
    return bsp_tfcard_init();
}

esp_err_t bsp_tfcard_format_and_mount(void)
{
    (void)bsp_tfcard_deinit();
    return sd_card_format();
}

esp_err_t bsp_tfcard_last_error(void)
{
    return s_last_err;
}

/* ---------------- 裸 SPI 探测（硬件取证）---------------- */

/** 每个半周期的延时：1us -> 约 400kHz，正好是 SD 卡初始化允许的上限频率 */
#define TF_PROBE_HALF_US        1U

/**
 * @brief  手动敲一个字节的 SPI（模式 0）
 * @note   上升沿：卡采样 MOSI、主机采样 MISO；下降沿：MOSI 换值
 */
static uint8_t tf_bitbang_xfer(uint8_t tx)
{
    uint8_t rx = 0;

    for (int i = 0; i < 8; i++) {
        gpio_set_level(BSP_SPI_MOSI_GPIO, (tx & 0x80U) ? 1 : 0);
        tx = (uint8_t)(tx << 1);

        esp_rom_delay_us(TF_PROBE_HALF_US);
        gpio_set_level(BSP_SPI_SCK_GPIO, 1);
        rx = (uint8_t)((rx << 1) | (gpio_get_level(BSP_SPI_MISO_GPIO) ? 1U : 0U));

        esp_rom_delay_us(TF_PROBE_HALF_US);
        gpio_set_level(BSP_SPI_SCK_GPIO, 0);
    }
    return rx;
}

/**
 * @brief  读卡的 R1 响应（最多等 8 个字节，第一个 bit7=0 的就是）
 */
static uint8_t tf_read_r1(void)
{
    uint8_t r = 0xFFU;

    for (int i = 0; i < 8; i++) {
        r = tf_bitbang_xfer(0xFFU);
        if ((r & 0x80U) == 0U) {
            break;
        }
    }
    return r;
}

/**
 * @brief  把 SPI 引脚从外设手里收回，配成普通 GPIO（CS/SCK/MOSI 输出，MISO 输入带上拉）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示引脚还被别的 SPI 器件占着
 */
static esp_err_t tf_gpio_take_over(void)
{
    esp_err_t err = bsp_spi_bus_deinit();

    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    const gpio_config_t out_cfg = {
        .pin_bit_mask = BSP_BIT(BSP_SPI_SCK_GPIO) | BSP_BIT(BSP_SPI_MOSI_GPIO) |
                        BSP_BIT(BSP_SPI_TF_CS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    const gpio_config_t in_cfg = {
        .pin_bit_mask = BSP_BIT(BSP_SPI_MISO_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 空槽时 MISO 悬空，上拉后恒为 1，便于判"无卡" */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    gpio_set_level(BSP_SPI_SCK_GPIO, 0);
    gpio_set_level(BSP_SPI_MOSI_GPIO, 1);
    gpio_set_level(BSP_SPI_TF_CS_GPIO, 1);      /* CS 高：先不选中 */
    return ESP_OK;
}

/**
 * @brief  把引脚还给 SPI 总线
 */
static void tf_gpio_release(void)
{
    (void)bsp_spi_bus_init();
}

esp_err_t bsp_tfcard_probe_raw(bsp_tfcard_probe_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sd_ready) {
        return ESP_ERR_INVALID_STATE;   /* 卡在服务中：引脚归块设备用，不能抢 */
    }
    memset(out, 0, sizeof(*out));

    /* ① 收回引脚（探测与上电唤醒都要先把 SPI 外设让开） */
    esp_err_t err = tf_gpio_take_over();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe: 引脚被占用 (%s)，无法探测", esp_err_to_name(err));
        return err;
    }

    /* ③ 至少 74 个时钟、CS 保持高，让卡进入 SPI 模式初始态 */
    for (int i = 0; i < 10; i++) {
        (void)tf_bitbang_xfer(0xFFU);
    }

    /* ④ CMD0 = GO_IDLE_STATE（0x40 00 00 00 00 95） */
    gpio_set_level(BSP_SPI_TF_CS_GPIO, 0);
    (void)tf_bitbang_xfer(0x40U);
    (void)tf_bitbang_xfer(0x00U);
    (void)tf_bitbang_xfer(0x00U);
    (void)tf_bitbang_xfer(0x00U);
    (void)tf_bitbang_xfer(0x00U);
    (void)tf_bitbang_xfer(0x95U);

    out->r1_idle = tf_read_r1();
    out->responded = ((out->r1_idle & 0x80U) == 0U);

    /* ⑤ CMD8 = SEND_IF_COND（0x48 00 00 01 AA 87），正常会回 4 字节 R7 = 00 00 01 AA */
    if (out->responded) {
        (void)tf_bitbang_xfer(0x48U);
        (void)tf_bitbang_xfer(0x00U);
        (void)tf_bitbang_xfer(0x00U);
        (void)tf_bitbang_xfer(0x01U);
        (void)tf_bitbang_xfer(0xAAU);
        (void)tf_bitbang_xfer(0x87U);

        (void)tf_read_r1();

        uint32_t tail = 0;

        for (int i = 0; i < 4; i++) {
            tail = (tail << 8) | tf_bitbang_xfer(0xFFU);
        }
        out->cmd8_tail = tail;
        out->cmd8_ok = (tail == 0x000001AAU);
    }

    /* ⑥ 收尾：CS 拉高，再补几个时钟让卡退出 */
    gpio_set_level(BSP_SPI_TF_CS_GPIO, 1);
    for (int i = 0; i < 2; i++) {
        (void)tf_bitbang_xfer(0xFFU);
    }

    /* ⑦ 引脚还给 SPI 总线，后续挂载照常 */
    tf_gpio_release();

    ESP_LOGI(TAG, "raw probe: R1=0x%02X CMD8=0x%08lX -> %s", (unsigned)out->r1_idle,
             (unsigned long)out->cmd8_tail,
             out->responded ? "card responded" : "NO response");
    return ESP_OK;
}

bool bsp_tfcard_is_mounted(void)
{
    return s_sd_ready;
}

esp_err_t bsp_tfcard_info(uint64_t *total_bytes, uint64_t *free_bytes,
                          char *fs_name, size_t fs_name_len)
{
    if (!s_sd_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    if (total_bytes != NULL || free_bytes != NULL) {
        uint64_t total = 0;
        uint64_t freeb = 0;

        err = esp_vfs_fat_info(BSP_TFCARD_MOUNT_POINT, &total, &freeb);
        if (err != ESP_OK) {
            return err;
        }
        if (total_bytes != NULL) {
            *total_bytes = total;
        }
        if (free_bytes != NULL) {
            *free_bytes = freeb;
        }
    }

    if (fs_name != NULL && fs_name_len > 0U) {
        (void)snprintf(fs_name, fs_name_len, "%s", "FAT");
    }

    return ESP_OK;
}

/* ================================================================
 * 自研 SD-SPI 块设备（绕开 IDF 的 SDSPI）
 *
 * 为什么自己写：
 *   同一张卡、同一组引脚，用普通 GPIO 手敲 SPI 时序能正常对话
 *   （CMD0 有 R1=0x01、CMD8 回显 0x1AA），而走 IDF 的 SDSPI（SPI 外设）
 *   却收不到任何应答，首发挂载必然报 ESP_ERR_NOT_FOUND。
 *   既然"手敲"这条路被实测证明可用，就把它直接做成块设备挂到 FATFS 上，
 *   不再依赖 SDSPI 的内部实现。对外 API（bsp_tfcard_*）完全不变。
 *
 * 性能说明：
 *   手敲 SPI 的上限在几百 kHz（受 GPIO 翻转与循环开销限制），适合 bringup
 *   阶段的功能验证与小文件读写。等把"外设通道为何收不到应答"查清（需要示波器看
 *   SCK/MISO 边沿），只需替换下面 sd_send()/sd_recv() 两个函数即可换成外设高速通道。
 * ================================================================ */

#define SD_BLOCK_SIZE       512U
#define SD_TOKEN_START      0xFEU
#define SD_DATA_RES_ACCEPT  0x05U
#define SD_TOKEN_TIMEOUT    3000        /* 等数据令牌最多读多少字节 */

/** 发送 n 字节 */
static void sd_send(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        (void)tf_bitbang_xfer(buf[i]);
    }
}

/** 接收 n 字节（发 0xFF 同时收） */
static void sd_recv(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = tf_bitbang_xfer(0xFFU);
    }
}

/** 空发 n 个 0xFF */
static void sd_send_ff(size_t n)
{
    while (n-- > 0U) {
        (void)tf_bitbang_xfer(0xFFU);
    }
}

/** CS 拉低（选中卡） */
static inline void sd_cs_low(void)
{
    gpio_set_level(BSP_SPI_TF_CS_GPIO, 0);
}

/** CS 拉高并补一个时钟，让卡释放 MISO */
static inline void sd_cs_high(void)
{
    gpio_set_level(BSP_SPI_TF_CS_GPIO, 1);
    (void)tf_bitbang_xfer(0xFFU);
}

/**
 * @brief  发一条 SD 命令并取回 R1
 * @param  cmd       命令号 0~63
 * @param  arg       32bit 参数
 * @param  extra     R3/R7 等附加数据缓冲，可 NULL
 * @param  extra_len 附加数据长度
 * @return R1：bit7=0 表示有应答；0xFF 表示无应答
 * @note   CMD0 在原生 SD 模式下 CRC 是强校验的，必须带 0x95；其余命令 SPI 模式不校验。
 */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t *extra, size_t extra_len)
{
    const uint8_t frame[6] = {
        (uint8_t)(0x40U | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        (cmd == 0U) ? 0x95U : 0x01U,
    };

    sd_cs_low();
    sd_send(frame, sizeof(frame));

    uint8_t r1 = 0xFFU;

    for (int i = 0; i < 8; i++) {                   /* NCR：最多等 8 字节 */
        r1 = tf_bitbang_xfer(0xFFU);
        if ((r1 & 0x80U) == 0U) {
            break;
        }
    }
    if (extra != NULL && extra_len > 0U) {
        sd_recv(extra, extra_len);
    }
    return r1;
}

/**
 * @brief  等数据起始令牌
 * @return true = 收到期望令牌
 */
static bool sd_wait_token(uint8_t token)
{
    for (int i = 0; i < SD_TOKEN_TIMEOUT; i++) {
        const uint8_t b = tf_bitbang_xfer(0xFFU);

        if (b == token) {
            return true;
        }
        if (b != 0xFFU) {                           /* 收到错误令牌 */
            return false;
        }
    }
    return false;
}

/** 单块读（CMD17） */
static bool sd_read_block(uint32_t sector, uint8_t *dst)
{
    const uint32_t addr = s_sd_sdhc ? sector : (sector * SD_BLOCK_SIZE);
    bool ok = false;

    if (sd_cmd(17U, addr, NULL, 0U) == 0x00U && sd_wait_token(SD_TOKEN_START)) {
        sd_recv(dst, SD_BLOCK_SIZE);
        sd_send_ff(2);                              /* CRC 不校验，读掉 */
        ok = true;
    }
    sd_cs_high();
    return ok;
}

/** 单块写（CMD24） */
static bool sd_write_block(uint32_t sector, const uint8_t *src)
{
    const uint32_t addr = s_sd_sdhc ? sector : (sector * SD_BLOCK_SIZE);
    bool ok = false;

    if (sd_cmd(24U, addr, NULL, 0U) == 0x00U) {
        (void)tf_bitbang_xfer(0xFFU);               /* 命令与令牌之间至少 1 字节 */
        (void)tf_bitbang_xfer(SD_TOKEN_START);
        sd_send(src, SD_BLOCK_SIZE);
        sd_send_ff(2);                              /* CRC 占位 */

        const uint8_t res = tf_bitbang_xfer(0xFFU);

        if ((res & 0x1FU) == SD_DATA_RES_ACCEPT) {
            for (int i = 0; i < 60000; i++) {       /* DO 被拉低表示卡在写 */
                if (tf_bitbang_xfer(0xFFU) != 0x00U) {
                    ok = true;
                    break;
                }
            }
        }
    }
    sd_cs_high();
    return ok;
}

/** 从 CSD 算总扇区数（兼容 CSD v1.0 / v2.0） */
static void sd_parse_csd(const uint8_t *csd)
{
    if ((csd[0] >> 6) == 1U) {                      /* v2.0：SDHC/SDXC */
        const uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16) |
                                ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];

        s_sd_sectors = (c_size + 1U) * 1024U;       /* 单位 512B 块 */
    } else {                                        /* v1.0：SDSC */
        const uint32_t read_bl_len = csd[5] & 0x0FU;
        const uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10) |
                                ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
        const uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03U) << 1) | ((uint32_t)csd[10] >> 7);
        const uint64_t bytes = ((uint64_t)c_size + 1ULL) *
                               (1ULL << (c_size_mult + 2U)) * (1ULL << read_bl_len);

        s_sd_sectors = (uint32_t)(bytes / SD_BLOCK_SIZE);
    }
}

/**
 * @brief  卡上电 + 进 SPI 模式 + 容量识别
 * @return ESP_OK 成功
 */
static esp_err_t sd_card_init(void)
{
    uint8_t buf[16];

    /* ① 上电后 ≥74 个时钟、CS 高（SD 规范 Part1 6.4.1.1） */
    sd_cs_high();
    sd_cs_high();
    sd_send_ff(10);

    /* ② CMD0：进入 SPI 空闲态 */
    if (sd_cmd(0U, 0U, NULL, 0U) != 0x01U) {
        sd_cs_high();
        return ESP_ERR_NOT_FOUND;
    }
    sd_cs_high();

    /* ③ CMD8：确认 2.7~3.6V 与 SDHC/SDXC（会回 0x1AA） */
    const bool v2 = (sd_cmd(8U, 0x1AAU, buf, 4U) == 0x01U) &&
                    (buf[2] == 0x01U) && (buf[3] == 0xAAU);

    sd_cs_high();

    /* ④ ACMD41：等卡退出空闲（HCS=1 表示支持高容量），最多约 1s */
    bool ready = false;

    for (int i = 0; i < 200; i++) {
        (void)sd_cmd(55U, 0U, NULL, 0U);
        sd_cs_high();

        const uint32_t arg = v2 ? 0x40000000U : 0U;

        if (sd_cmd(41U, arg, NULL, 0U) == 0x00U) {
            ready = true;
            sd_cs_high();
            break;
        }
        sd_cs_high();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!ready) {
        return ESP_ERR_TIMEOUT;
    }

    /* ⑤ CMD58：读 OCR，CCS=1 表示按块寻址 */
    s_sd_sdhc = false;
    if (sd_cmd(58U, 0U, buf, 4U) == 0x00U) {
        s_sd_sdhc = ((buf[0] & 0x40U) != 0U);
    }
    sd_cs_high();

    /* ⑥ SDSC 必须显式把块长度设成 512 */
    if (!s_sd_sdhc) {
        (void)sd_cmd(16U, SD_BLOCK_SIZE, NULL, 0U);
        sd_cs_high();
    }

    /* ⑦ CMD9：读 CSD 得到容量 */
    s_sd_sectors = 0;
    if (sd_cmd(9U, 0U, NULL, 0U) == 0x00U && sd_wait_token(SD_TOKEN_START)) {
        sd_recv(buf, 16U);
        sd_send_ff(2);
        sd_parse_csd(buf);
    }
    sd_cs_high();

    return (s_sd_sectors > 0U) ? ESP_OK : ESP_FAIL;
}

/* ---------------- FATFS diskio 接口 ---------------- */

static DSTATUS sd_disk_initialize(BYTE pdrv)
{
    BSP_UNUSED(pdrv);
    return (s_sd_sectors > 0U) ? 0 : STA_NOINIT;
}

static DSTATUS sd_disk_status(BYTE pdrv)
{
    BSP_UNUSED(pdrv);
    return 0;
}

static DRESULT sd_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    BSP_UNUSED(pdrv);

    for (UINT i = 0; i < count; i++) {
        if (!sd_read_block(sector + i, buff + (size_t)i * SD_BLOCK_SIZE)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT sd_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    BSP_UNUSED(pdrv);

    for (UINT i = 0; i < count; i++) {
        if (!sd_write_block(sector + i, buff + (size_t)i * SD_BLOCK_SIZE)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    BSP_UNUSED(pdrv);

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = (DWORD)s_sd_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)SD_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1U;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_sd_diskio = {
    .init   = &sd_disk_initialize,
    .status = &sd_disk_status,
    .read   = &sd_disk_read,
    .write  = &sd_disk_write,
    .ioctl  = &sd_disk_ioctl,
};

/** 用 f_mkfs 重建 FAT 分区（工作区放内部 RAM） */
static FRESULT sd_format_fs(void)
{
    static uint8_t work[4096];

    /* 显式指定 FAT32 且**不带** FM_SFD —— 即建立 MBR 分区表 + 一个 FAT32 分区，
     * 这是 SD 卡在电脑/相机上通用的布局（不指定时 FatFs 会走"超级软盘"布局）。 */
    const MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 0,
        .align = 0,
        .n_root = 0,
        .au_size = 0,
    };

    return f_mkfs(s_drive, &opt, work, sizeof(work));
}

/**
 * @brief  挂载：接管引脚 -> 卡初始化 -> 注册 diskio/VFS -> f_mount（必要时格式化）
 */
static esp_err_t sd_card_mount(void)
{
    if (tf_gpio_take_over() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;               /* 引脚被别的 SPI 器件占着 */
    }

    esp_err_t err = sd_card_init();

    if (err != ESP_OK) {
        tf_gpio_release();
        return err;
    }

    BYTE pdrv = FF_DRV_NOT_USED;

    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        tf_gpio_release();
        return ESP_ERR_NO_MEM;
    }
    s_pdrv = pdrv;
    s_drive[0] = (char)('0' + (int)pdrv);           /* 卷号必须与 pdrv 一致 */

    ff_diskio_register(s_pdrv, &s_sd_diskio);

    err = esp_vfs_fat_register(BSP_TFCARD_MOUNT_POINT, s_drive, (size_t)TF_MAX_FILES, &s_fs);
    if (err != ESP_OK) {
        ff_diskio_register(s_pdrv, NULL);
        tf_gpio_release();
        s_fs = NULL;
        return err;
    }

    FRESULT res = f_mount(s_fs, s_drive, 1);

    if (res != FR_OK && s_format_if_failed) {
        ESP_LOGW(TAG, "f_mount 失败(%d)，按要求格式化重建 FAT", (int)res);
        res = sd_format_fs();
        if (res == FR_OK) {
            res = f_mount(s_fs, s_drive, 1);
        }
    }

    if (res != FR_OK) {
        ESP_LOGW(TAG, "f_mount failed: %d", (int)res);
        (void)esp_vfs_fat_unregister_path(BSP_TFCARD_MOUNT_POINT);
        ff_diskio_register(s_pdrv, NULL);
        s_fs = NULL;
        s_pdrv = 0xFF;
        tf_gpio_release();
        return ESP_FAIL;
    }

    s_sd_ready = true;

    uint64_t total = 0;
    uint64_t freeb = 0;

    if (esp_vfs_fat_info(BSP_TFCARD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s: %lluMB / %lluMB free (%s, 自研 bit-bang SPI)",
                 BSP_TFCARD_MOUNT_POINT, (unsigned long long)(total >> 20),
                 (unsigned long long)(freeb >> 20), s_sd_sdhc ? "SDHC+" : "SDSC");
    }
    return ESP_OK;
}

/** 卸载：反挂载 + 注销 + 还引脚 */
static void sd_card_unmount(void)
{
    if (s_fs != NULL) {
        (void)f_mount(NULL, s_drive, 0);
        (void)esp_vfs_fat_unregister_path(BSP_TFCARD_MOUNT_POINT);
        s_fs = NULL;
    }
    ff_diskio_register(s_pdrv, NULL);
    s_pdrv = 0xFF;
    s_sd_ready = false;
    s_sd_sectors = 0;
    tf_gpio_release();
}

/** "格式化并挂载"：借用 format_if_mount_failed 开关跑一次挂载 */
static esp_err_t sd_card_format(void)
{
    const bool saved = s_format_if_failed;

    s_format_if_failed = true;
    const esp_err_t err = sd_card_mount();
    s_format_if_failed = saved;

    return err;
}
