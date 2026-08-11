/* ================================================================
 * bsp_touch —— 触摸屏底层驱动：SPI 读坐标
 *
 * 架构位置：APP BSP 层；touch_svc 服务依赖
 * ================================================================ */
#include "bsp_touch.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ================================================================
 * XPT2046 位操作 SPI 实现
 * ================================================================ */

/* ---------- 引脚定义（探索者V3） ---------- */
#define T_CLK_PORT  GPIOB
#define T_CLK_PIN   GPIO_PIN_0
#define T_PEN_PORT  GPIOB
#define T_PEN_PIN   GPIO_PIN_1
#define T_MISO_PORT GPIOB
#define T_MISO_PIN  GPIO_PIN_2
#define T_CS_PORT   GPIOC
#define T_CS_PIN    GPIO_PIN_13
#define T_MOSI_PORT GPIOF
#define T_MOSI_PIN  GPIO_PIN_11

/* ---------- 时序参数（168MHz） ---------- */
/* 1MHz SPI：弱接触（高源阻抗）时采样保持电容需要更长获取时间，
 * 慢速 SCLK 显著改善轻触读数稳定性。 */
#define TOUCH_HALF_CLK_CYCLES   84u   /* ~500ns 半周期 => ~1MHz SPI */
#define TOUCH_CONV_US           6u    /* XPT2046 转换时间最长 6us */

/* ---------- 采样滤波 ---------- */
#define TOUCH_READ_TIMES        5     /* 单轴采样次数 */
#define TOUCH_LOST_VAL          1     /* 去极值个数（两端各 1） */

/* ---------- 通道命令（S=1, 12bit, SER/DFR, PD=00） ---------- */
#define CMD_X_POS               0xD0  /* X 通道 */
#define CMD_Y_POS               0x90  /* Y 通道 */

static bsp_touch_cal_t s_cal = {
    .xfac = 18, .yfac = 12,
    .xc = 2048, .yc = 2048,
    .valid = 1,      /* 出厂默认近似值，建议运行 touch cal 精校准 */
};
static SemaphoreHandle_t s_io_lock = NULL;   /* 串行化 SPI 访问（采样/校准） */

/* ---------- 微秒/半时钟延时（DWT，168MHz） ---------- */
static void touch_delay_cycles(uint32_t cyc)
{
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cyc) {
    }
}

/* ---------- 位操作原语 ---------- */
static void clk_hi(void)  { HAL_GPIO_WritePin(T_CLK_PORT, T_CLK_PIN, GPIO_PIN_SET); }
static void clk_lo(void)  { HAL_GPIO_WritePin(T_CLK_PORT, T_CLK_PIN, GPIO_PIN_RESET); }
static void mosi(uint8_t v){ HAL_GPIO_WritePin(T_MOSI_PORT, T_MOSI_PIN,
                               v ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static void cs(uint8_t v) { HAL_GPIO_WritePin(T_CS_PORT, T_CS_PIN,
                               v ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static uint8_t miso(void) { return (HAL_GPIO_ReadPin(T_MISO_PORT, T_MISO_PIN)
                                    == GPIO_PIN_SET) ? 1u : 0u; }
static uint8_t pen(void)  { return (HAL_GPIO_ReadPin(T_PEN_PORT, T_PEN_PIN)
                                    == GPIO_PIN_RESET) ? 1u : 0u; }

/* ---------- SPI 写 1 字节（上升沿锁存） ---------- */
static void tp_write_byte(uint8_t data)
{
    for (uint8_t i = 0; i < 8; i++) {
        mosi((data & 0x80u) ? 1u : 0u);
        data <<= 1;
        clk_lo();
        touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
        clk_hi();
        touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
    }
}

/* ---------- 读 16 位 ADC（高 12 位有效，下降沿采样） ---------- */
static uint16_t tp_read_ad(uint8_t cmd)
{
    uint16_t num = 0;

    clk_lo();
    mosi(0);
    cs(0);                       /* 选中 */
    tp_write_byte(cmd);          /* 8 位命令 */
    touch_delay_cycles(TOUCH_CONV_US * 168u);   /* 转换时间 */

    clk_lo();
    touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
    clk_hi();                    /* 清 BUSY */
    touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
    clk_lo();

    for (uint8_t i = 0; i < 16; i++) {
        clk_lo();
        touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
        clk_hi();
        num = (uint16_t)((num << 1) | miso());
        touch_delay_cycles(TOUCH_HALF_CLK_CYCLES);
    }

    num >>= 4;                   /* 高 12 位有效 */
    cs(1);
    return num;
}

/* ---------- 单轴滤波读取（5 次去极值均值） ---------- */
static uint16_t tp_read_xoy(uint8_t cmd)
{
    uint16_t buf[TOUCH_READ_TIMES];

    for (uint8_t i = 0; i < TOUCH_READ_TIMES; i++) {
        buf[i] = tp_read_ad(cmd);
    }
    /* 升序排序 */
    for (uint8_t i = 0; i < TOUCH_READ_TIMES - 1; i++) {
        for (uint8_t j = i + 1; j < TOUCH_READ_TIMES; j++) {
            if (buf[i] > buf[j]) {
                uint16_t t = buf[i];
                buf[i] = buf[j];
                buf[j] = t;
            }
        }
    }
    uint32_t sum = 0;
    for (uint8_t i = TOUCH_LOST_VAL; i < TOUCH_READ_TIMES - TOUCH_LOST_VAL; i++) {
        sum += buf[i];
    }
    return (uint16_t)(sum / (TOUCH_READ_TIMES - 2u * TOUCH_LOST_VAL));
}

/* ---------- 双次校验读取（仅诊断/校准用） ---------- */
static uint8_t tp_read_xy_valid(int32_t *rx, int32_t *ry)
{
    int32_t x1 = tp_read_xoy(CMD_X_POS);
    int32_t y1 = tp_read_xoy(CMD_Y_POS);
    int32_t x2 = tp_read_xoy(CMD_X_POS);
    int32_t y2 = tp_read_xoy(CMD_Y_POS);

    if (x1 < 0 || x1 > 4095 || y1 < 0 || y1 > 4095 ||
        x2 < 0 || x2 > 4095 || y2 < 0 || y2 > 4095) {
        return 0;
    }
    if (((x2 <= x1 && x1 < x2 + 80) || (x1 <= x2 && x2 < x1 + 80)) &&
        ((y2 <= y1 && y1 < y2 + 80) || (y1 <= y2 && y2 < y1 + 80))) {
        *rx = (x1 + x2) / 2;
        *ry = (y1 + y2) / 2;
        return 1;
    }
    return 0;
}

/* 有效触点判定范围：无触摸时 X/Y 读近 4095 或 0，触点有效值在中段 */
#define TOUCH_RAW_MIN    5     /* 放宽阈值：极轻触（边缘弱接触）也能识别 */
#define TOUCH_RAW_MAX    4090

static uint8_t tp_raw_valid(int32_t v)
{
    return (v > TOUCH_RAW_MIN && v < TOUCH_RAW_MAX);
}

/* 3 次采样中值（快速探测用：比单次稳，比 5 次快） */
static uint16_t tp_read_xoy3(uint8_t cmd)
{
    uint16_t a = tp_read_ad(cmd);
    uint16_t b = tp_read_ad(cmd);
    uint16_t c = tp_read_ad(cmd);
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;   /* 中值 */
}

/* ================================================================
 * BSP 接口
 * ================================================================ */

void BSP_Touch_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* 输入：T_PEN(PB1) / T_MISO(PB2)，上拉 */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Pin = T_PEN_PIN | T_MISO_PIN;
    HAL_GPIO_Init(T_PEN_PORT, &gpio);

    /* 输出：T_CLK(PB0) / T_MOSI(PF11) / T_CS(PC13) */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin = T_CLK_PIN;
    HAL_GPIO_Init(T_CLK_PORT, &gpio);
    gpio.Pin = T_MOSI_PIN;
    HAL_GPIO_Init(T_MOSI_PORT, &gpio);
    gpio.Pin = T_CS_PIN;
    HAL_GPIO_Init(T_CS_PORT, &gpio);

    clk_lo();
    cs(1);
    mosi(0);

    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutex();
    }
}

uint8_t BSP_Touch_Pressed(void)
{
    return pen();
}

uint8_t BSP_Touch_ReadRaw(int32_t *rx, int32_t *ry)
{
    uint8_t ok = 0;
    if (s_io_lock == NULL ||
        xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* 快速探测：3 次中值 X 读超范围（无触摸）立即返回，省大部分开销 */
        int32_t x1 = tp_read_xoy3(CMD_X_POS);
        if (tp_raw_valid(x1)) {
            /* 全量：X/Y 中值滤波 + 触点范围判定（不做双次校验，
             * 轻触抖动不误拒，噪声由上层平滑吸收） */
            *rx = tp_read_xoy(CMD_X_POS);
            *ry = tp_read_xoy(CMD_Y_POS);
            ok = (uint8_t)(tp_raw_valid(*rx) && tp_raw_valid(*ry));
        }
        if (s_io_lock != NULL) xSemaphoreGive(s_io_lock);
    }
    return ok;
}

/* 无条件完整读取（诊断/校准用，不受触点判定范围限制） */
uint8_t BSP_Touch_ReadRawForce(int32_t *rx, int32_t *ry)
{
    uint8_t ok = 0;
    if (s_io_lock == NULL ||
        xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        ok = tp_read_xy_valid(rx, ry);
        if (s_io_lock != NULL) xSemaphoreGive(s_io_lock);
    }
    return ok;
}

void BSP_Touch_Convert(int32_t rx, int32_t ry, uint16_t *lx, uint16_t *ly)
{
    int32_t x = 0, y = 0;
    uint16_t w = 240, h = 320;

    if (s_cal.xfac > 0) x = (rx - s_cal.xc) / s_cal.xfac + (int32_t)w / 2;
    if (s_cal.yfac > 0) y = (ry - s_cal.yc) / s_cal.yfac + (int32_t)h / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int32_t)w) x = (int32_t)w - 1;
    if (y >= (int32_t)h) y = (int32_t)h - 1;
    *lx = (uint16_t)x;
    *ly = (uint16_t)y;
}

void BSP_Touch_SetCal(const bsp_touch_cal_t *cal)
{
    if (cal == NULL) return;
    s_cal = *cal;
}

void BSP_Touch_GetCal(bsp_touch_cal_t *cal)
{
    if (cal != NULL) *cal = s_cal;
}
