/* ================================================================
 * bsp_mpu6050 —— MPU6050 底层驱动：I2C 读写/自检
 *
 * 架构位置：APP BSP 层；imu_svc 服务依赖
 * ================================================================ */
#include "bsp_mpu6050.h"
#include "bsp_i2c.h"
#include "i2c.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* ================================================================
 * MPU6050 驱动实现
 * ================================================================ */

#define MPU6050_ADDR      (0x68u << 1)
#define MPU6050_ADDR_ALT  (0x69u << 1)

#define REG_SMPLRT_DIV    0x19
#define REG_CONFIG        0x1A
#define REG_GYRO_CONFIG   0x1B
#define REG_ACCEL_CONFIG  0x1C
#define REG_PWR_MGMT_1    0x6B
#define REG_ACCEL_XOUT_H  0x3B
#define REG_WHO_AM_I      0x75

#define I2C_TIMEOUT       100

static uint16_t s_addr = MPU6050_ADDR;
static mpu6050_cal_t s_cal;
static SemaphoreHandle_t s_i2c_done = NULL;  /* IT 传输完成信号（ISR 给） */
static volatile uint8_t s_i2c_err = 0;       /* 完成时是否伴随错误 */

/* ---------------- I2C IT 完成回调（仅本驱动使用 I2C1） ---------------- */
static void i2c_it_done(I2C_HandleTypeDef *hi2c, uint8_t err)
{
    if (hi2c->Instance == I2C1 && s_i2c_done != NULL) {
        BaseType_t w = pdFALSE;
        s_i2c_err = err;
        xSemaphoreGiveFromISR(s_i2c_done, &w);
        portYIELD_FROM_ISR(w);
    }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) { i2c_it_done(hi2c, 0); }
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c) { i2c_it_done(hi2c, 0); }
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)     { i2c_it_done(hi2c, 1); }

/* ---------- 寄存器读写（带互斥 + 总线自恢复） ---------- */
static uint8_t reg_op(uint8_t reg, uint8_t *data, uint16_t len, uint8_t is_write)
{
    uint8_t ok = 1;
    if (BSP_I2C1_Lock(50) == 0) {
        HAL_StatusTypeDef st;
        /* IT 模式优先：任务在传输期间休眠，CPU 占用从 ~8% 降到 ~1% */
        if (s_i2c_done == NULL) {
            s_i2c_done = xSemaphoreCreateBinary();
            if (s_i2c_done == NULL) {
                BSP_I2C1_Unlock();
                return ok;
            }
        }
        (void)xSemaphoreTake(s_i2c_done, 0);   /* 清残留信号 */
        s_i2c_err = 0;
        if (is_write) {
            st = HAL_I2C_Mem_Write_IT(&hi2c1, s_addr, reg, I2C_MEMADD_SIZE_8BIT,
                                      data, len);
        } else {
            st = HAL_I2C_Mem_Read_IT(&hi2c1, s_addr, reg, I2C_MEMADD_SIZE_8BIT,
                                     data, len);
        }
        if (st == HAL_OK &&
            xSemaphoreTake(s_i2c_done, pdMS_TO_TICKS(I2C_TIMEOUT)) == pdTRUE &&
            !s_i2c_err) {
            ok = 0;
        }
        if (ok) {
            /* IT 启动失败/超时：回退阻塞模式 + 总线自恢复重试一次 */
            HAL_I2C_DeInit(&hi2c1);
            HAL_I2C_Init(&hi2c1);
            if (is_write) {
                st = HAL_I2C_Mem_Write(&hi2c1, s_addr, reg, I2C_MEMADD_SIZE_8BIT,
                                       data, len, I2C_TIMEOUT);
            } else {
                st = HAL_I2C_Mem_Read(&hi2c1, s_addr, reg, I2C_MEMADD_SIZE_8BIT,
                                      data, len, I2C_TIMEOUT);
            }
            if (st == HAL_OK) ok = 0;
        }
        BSP_I2C1_Unlock();
    }
    return ok;
}

static uint8_t reg_write(uint8_t reg, uint8_t val)
{
    return reg_op(reg, &val, 1, 1);
}

static uint8_t reg_read(uint8_t reg, uint8_t *val)
{
    return reg_op(reg, val, 1, 0);
}

/* ---------- 接口 ---------- */
uint8_t BSP_MPU6050_Check(void)
{
    uint8_t id = 0;
    if (reg_read(REG_WHO_AM_I, &id)) return 1;
    return (id == 0x68) ? 0 : 1;
}

uint8_t BSP_MPU6050_Init(void)
{
    BSP_I2C1_Init();

    /* I2C1 事件/错误中断（IT 模式必需）：优先级 7（≥5 可安全 FromISR） */
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

    s_addr = MPU6050_ADDR;
    if (BSP_MPU6050_Check()) {
        s_addr = MPU6050_ADDR_ALT;
        if (BSP_MPU6050_Check()) return 1;
    }

    /* 复位器件 */
    reg_write(REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    /* 退出睡眠，时钟源=PLL X 轴陀螺（最高精度） */
    reg_write(REG_PWR_MGMT_1, 0x01);
    /* 采样率 = 1kHz/(1+4) = 200Hz；DLPF=98Hz 带宽（硬件抗混叠） */
    reg_write(REG_SMPLRT_DIV, 4);
    reg_write(REG_CONFIG, 0x02);
    /* 量程：±250dps（131 LSB/dps）/ ±2g（16384 LSB/g）最高分辨率 */
    reg_write(REG_GYRO_CONFIG, 0x00);
    reg_write(REG_ACCEL_CONFIG, 0x00);
    vTaskDelay(pdMS_TO_TICKS(50));
    return BSP_MPU6050_Check();
}

uint8_t BSP_MPU6050_ReadRaw(mpu6050_raw_t *raw)
{
    uint8_t buf[14];
    if (raw == NULL) return 1;
    if (reg_op(REG_ACCEL_XOUT_H, buf, 14, 0)) return 1;
    raw->ax  = (int16_t)((buf[0] << 8) | buf[1]);
    raw->ay  = (int16_t)((buf[2] << 8) | buf[3]);
    raw->az  = (int16_t)((buf[4] << 8) | buf[5]);
    raw->temp = (int16_t)((buf[6] << 8) | buf[7]);
    raw->gx  = (int16_t)((buf[8] << 8) | buf[9]);
    raw->gy  = (int16_t)((buf[10] << 8) | buf[11]);
    raw->gz  = (int16_t)((buf[12] << 8) | buf[13]);
    return 0;
}

void BSP_MPU6050_Calibrate(const uint16_t samples)
{
    int64_t sx = 0, sy = 0, sz = 0, sax = 0, say = 0, saz = 0;
    uint16_t n = 0;
    mpu6050_raw_t raw;

    for (uint16_t i = 0; i < samples; i++) {
        if (BSP_MPU6050_ReadRaw(&raw) == 0) {
            sx += raw.gx;  sy += raw.gy;  sz += raw.gz;
            sax += raw.ax; say += raw.ay; saz += raw.az;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (n == 0) return;

    s_cal.gx_bias = (float)sx / n / MPU6050_GYRO_SCALE;
    s_cal.gy_bias = (float)sy / n / MPU6050_GYRO_SCALE;
    s_cal.gz_bias = (float)sz / n / MPU6050_GYRO_SCALE;

    /* 加速度偏移：仅当校准姿态近似水平（|a|≈1g）时应用，否则清零 */
    float amx = (float)sax / n / MPU6050_ACCEL_SCALE;
    float amy = (float)say / n / MPU6050_ACCEL_SCALE;
    float amz = (float)saz / n / MPU6050_ACCEL_SCALE;
    float mag = amx * amx + amy * amy + amz * amz;
    if (mag > 0.90f && mag < 1.10f) {
        s_cal.ax_off = amx;
        s_cal.ay_off = amy;
        s_cal.az_off = amz - 1.0f;
    } else {
        s_cal.ax_off = s_cal.ay_off = s_cal.az_off = 0.0f;
    }
    s_cal.valid = 1;
}

void BSP_MPU6050_GetCal(mpu6050_cal_t *cal)
{
    if (cal != NULL) *cal = s_cal;
}
