/* ================================================================
 * imu.c —— QMI8658A 驱动
 *
 * 寄存器（与厂商 BSP 的 enum 完全一致）：
 *   0x00 WHO_AM_I(0x05)   0x01 Revision   0x02 CTRL1   0x03 CTRL2(加速度)
 *   0x04 CTRL3(陀螺仪)    0x06 CTRL5      0x08 CTRL7(使能)  0x0B~0x10 校准
 *   0x33 温度(2B)         0x35~0x40 加速度+角速度(12B)
 *   0x60 Reset(写 0xB0 复位)
 *
 * 编码规则：
 *   CTRL2 = (acc_range_code << 4) | acc_odr_code
 *   CTRL3 = (gyro_range_code << 4) | gyro_odr_code
 *   CTRL7 = bit0 加速度使能 | bit1 陀螺仪使能
 * ================================================================ */
#include "bsp/imu.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/bus/i2c_bus.h"
#include "bsp/xl9555.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP/IMU";

/* ---------------- 寄存器 ---------------- */

#define QMI_REG_WHOAMI          0x00U
#define QMI_REG_REVISION        0x01U
#define QMI_REG_CTRL1           0x02U
#define QMI_REG_CTRL2           0x03U
#define QMI_REG_CTRL3           0x04U
#define QMI_REG_CTRL7           0x08U
#define QMI_REG_TEMP_L          0x33U
#define QMI_REG_AX_L            0x35U
#define QMI_REG_RESET           0x60U

#define QMI_WHOAMI_VALUE        0x05U
#define QMI_RESET_CMD           0xB0U
#define QMI_CTRL1_AUTO_INC      0x60U    /* 与厂商例程一致：打开地址自增 */

#define QMI_RESET_DELAY_MS      30U
#define QMI_CTRL7_ACC_EN        0x01U
#define QMI_CTRL7_GYR_EN        0x02U

/* ---------------- 编码表 ---------------- */

typedef struct
{
    uint16_t value;     /*!< 物理值（g 或 dps 或 Hz） */
    uint8_t  code;      /*!< 寄存器编码 */
} qmi_code_t;

static const qmi_code_t s_acc_range_map[] = {
    {2U, 0x00U}, {4U, 0x01U}, {8U, 0x02U}, {16U, 0x03U},
};

static const qmi_code_t s_gyro_range_map[] = {
    {16U, 0x00U}, {32U, 0x01U}, {64U, 0x02U}, {128U, 0x03U},
    {256U, 0x04U}, {512U, 0x05U}, {1024U, 0x06U}, {2048U, 0x07U},
};

static const qmi_code_t s_odr_map[] = {
    {8000U, 0x00U}, {4000U, 0x01U}, {2000U, 0x02U}, {1000U, 0x03U},
    {500U, 0x04U}, {250U, 0x05U}, {125U, 0x06U}, {62U, 0x07U}, {31U, 0x08U},
};

/* ---------------- 状态 ---------------- */

static bsp_i2c_dev_t s_dev = NULL;
static bsp_imu_cfg_t s_cfg = BSP_IMU_CFG_DEFAULT;
static float         s_lsb_per_g = 0.0f;
static float         s_lsb_per_dps = 0.0f;
static bool          s_ready = false;

/* ---------------- 内部工具 ---------------- */

/**
 * @brief  在编码表里查找物理值对应的寄存器编码
 */
static bool qmi_lookup(const qmi_code_t *map, size_t n, uint16_t value, uint8_t *code)
{
    for (size_t i = 0; i < n; i++) {
        if (map[i].value == value) {
            *code = map[i].code;
            return true;
        }
    }
    return false;
}

/**
 * @brief  把配置编码成 CTRL2/CTRL3 的值
 */
static esp_err_t qmi_encode(const bsp_imu_cfg_t *cfg, uint8_t *ctrl2, uint8_t *ctrl3)
{
    uint8_t acc_range = 0;
    uint8_t acc_odr = 0;
    uint8_t gyr_range = 0;
    uint8_t gyr_odr = 0;

    if (!qmi_lookup(s_acc_range_map, BSP_COUNT_OF(s_acc_range_map), cfg->acc_range_g, &acc_range) ||
        !qmi_lookup(s_odr_map, BSP_COUNT_OF(s_odr_map), cfg->acc_odr_hz, &acc_odr) ||
        !qmi_lookup(s_gyro_range_map, BSP_COUNT_OF(s_gyro_range_map), cfg->gyro_range_dps, &gyr_range) ||
        !qmi_lookup(s_odr_map, BSP_COUNT_OF(s_odr_map), cfg->gyro_odr_hz, &gyr_odr)) {
        return ESP_ERR_INVALID_ARG;
    }

    *ctrl2 = (uint8_t)((acc_range << 4) | acc_odr);
    *ctrl3 = (uint8_t)((gyr_range << 4) | gyr_odr);
    return ESP_OK;
}

/* ---------------- 配置 ---------------- */

esp_err_t bsp_imu_configure(const bsp_imu_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t ctrl2 = 0;
    uint8_t ctrl3 = 0;
    esp_err_t err = qmi_encode(cfg, &ctrl2, &ctrl3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unsupported range/odr: %ug %uHz %udps %uHz",
                 (unsigned)cfg->acc_range_g, (unsigned)cfg->acc_odr_hz,
                 (unsigned)cfg->gyro_range_dps, (unsigned)cfg->gyro_odr_hz);
        return err;
    }

    s_cfg = *cfg;
    return ESP_OK;
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_imu_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_add_device(BSP_QMI8658A_I2C_ADDR, BSP_I2C_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* ① 复位：写 0xB0 再写 0x00（厂商例程同款流程） */
    err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_RESET, QMI_RESET_CMD);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(QMI_RESET_DELAY_MS));
        err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_RESET, 0x00U);
    }
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ② 校验 WHO_AM_I */
    uint8_t whoami = 0;
    uint8_t revision = 0;

    err = bsp_i2c_reg_read_u8(s_dev, QMI_REG_WHOAMI, &whoami);
    if (err == ESP_OK) {
        err = bsp_i2c_reg_read_u8(s_dev, QMI_REG_REVISION, &revision);
    }
    if (err != ESP_OK) {
        goto fail;
    }
    if (whoami != QMI_WHOAMI_VALUE) {
        ESP_LOGE(TAG, "bad WHO_AM_I: 0x%02X (expect 0x%02X)",
                 (unsigned)whoami, (unsigned)QMI_WHOAMI_VALUE);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    /* ③ 配置 */
    uint8_t ctrl2 = 0;
    uint8_t ctrl3 = 0;
    err = qmi_encode(&s_cfg, &ctrl2, &ctrl3);
    if (err != ESP_OK) {
        goto fail;
    }

    err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL7, 0x00U);        /* 先关 */
    if (err == ESP_OK) {
        err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL1, QMI_CTRL1_AUTO_INC);
    }
    if (err == ESP_OK) {
        err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL2, ctrl2);
    }
    if (err == ESP_OK) {
        err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL3, ctrl3);
    }
    if (err == ESP_OK) {
        err = bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL7,
                                   QMI_CTRL7_ACC_EN | QMI_CTRL7_GYR_EN);
    }
    if (err != ESP_OK) {
        goto fail;
    }

    /* ④ 灵敏度：16bit 有符号覆盖 ±满量程 */
    s_lsb_per_g = 32768.0f / (float)s_cfg.acc_range_g;
    s_lsb_per_dps = 32768.0f / (float)s_cfg.gyro_range_dps;
    s_ready = true;

    ESP_LOGI(TAG, "ready (WHO=0x%02X REV=0x%02X, ±%ug/±%udps @%uHz)",
             (unsigned)whoami, (unsigned)revision, (unsigned)s_cfg.acc_range_g,
             (unsigned)s_cfg.gyro_range_dps, (unsigned)s_cfg.acc_odr_hz);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    (void)bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t bsp_imu_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }

    (void)bsp_i2c_reg_write_u8(s_dev, QMI_REG_CTRL7, 0x00U);    /* 关闭传感器 */

    esp_err_t err = bsp_i2c_rm_device(s_dev);
    s_dev = NULL;
    s_ready = false;
    return err;
}

/* ---------------- 读取 ---------------- */

esp_err_t bsp_imu_read_raw(bsp_imu_raw_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[12] = {0};

    esp_err_t err = bsp_i2c_reg_read(s_dev, QMI_REG_AX_L, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < 3; i++) {
        out->acc[i] = (int16_t)(((uint16_t)buf[i * 2 + 1] << 8) | buf[i * 2]);
        out->gyro[i] = (int16_t)(((uint16_t)buf[i * 2 + 7] << 8) | buf[i * 2 + 6]);
    }

    uint8_t tbuf[2] = {0};
    err = bsp_i2c_reg_read(s_dev, QMI_REG_TEMP_L, tbuf, sizeof(tbuf));
    if (err == ESP_OK) {
        out->temp = (int16_t)(((uint16_t)tbuf[1] << 8) | tbuf[0]);
    }
    return err;
}

esp_err_t bsp_imu_read(bsp_imu_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_imu_raw_t raw;
    esp_err_t err = bsp_imu_read_raw(&raw);
    if (err != ESP_OK) {
        return err;
    }

    out->acc_g.x = (float)raw.acc[0] / s_lsb_per_g;
    out->acc_g.y = (float)raw.acc[1] / s_lsb_per_g;
    out->acc_g.z = (float)raw.acc[2] / s_lsb_per_g;

    out->gyro_dps.x = (float)raw.gyro[0] / s_lsb_per_dps;
    out->gyro_dps.y = (float)raw.gyro[1] / s_lsb_per_dps;
    out->gyro_dps.z = (float)raw.gyro[2] / s_lsb_per_dps;

    out->temp_c = (float)raw.temp / 256.0f;
    return ESP_OK;
}

esp_err_t bsp_imu_read_acc(bsp_vec3_t *acc_g)
{
    if (acc_g == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_imu_raw_t raw;
    esp_err_t err = bsp_imu_read_raw(&raw);
    if (err != ESP_OK) {
        return err;
    }

    acc_g->x = (float)raw.acc[0] / s_lsb_per_g;
    acc_g->y = (float)raw.acc[1] / s_lsb_per_g;
    acc_g->z = (float)raw.acc[2] / s_lsb_per_g;
    return ESP_OK;
}

esp_err_t bsp_imu_read_temperature(float *temp_c)
{
    if (temp_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_imu_raw_t raw;
    esp_err_t err = bsp_imu_read_raw(&raw);
    if (err == ESP_OK) {
        *temp_c = (float)raw.temp / 256.0f;
    }
    return err;
}

bool bsp_imu_irq_asserted(void)
{
    return !bsp_xl9555_pin_read(BSP_XL9555_PIN_QMA_INT);
}

esp_err_t bsp_imu_read_id(uint8_t *whoami, uint8_t *revision)
{
    if (s_dev == NULL || whoami == NULL || revision == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = bsp_i2c_reg_read_u8(s_dev, QMI_REG_WHOAMI, whoami);
    if (err == ESP_OK) {
        err = bsp_i2c_reg_read_u8(s_dev, QMI_REG_REVISION, revision);
    }
    return err;
}

esp_err_t bsp_imu_write_reg(uint8_t reg, uint8_t val)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_i2c_reg_write_u8(s_dev, reg, val);
}

esp_err_t bsp_imu_read_reg(uint8_t reg, uint8_t *val, size_t len)
{
    if (s_dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_i2c_reg_read(s_dev, reg, val, len);
}
