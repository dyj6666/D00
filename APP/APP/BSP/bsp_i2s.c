/* ================================================================
 * bsp_i2s —— I2S2 音频接口驱动实现
 *
 * 移植自正点原子探索者 F407 例程（实验43 音乐播放器实验 i2s.c），
 * 精简：去除 SD 卡/文件依赖，仅保留 I2S2 主机 TX + DMA 双缓冲播放。
 *
 * 采样率公式：Fs = I2SxCLK / [256 × (2×I2SDIV + ODD)]
 *   I2SxCLK = (HSE/PLLM) × PLLI2SN / PLLI2SR
 *   （HSE=8MHz, PLLM=8 → VCO 输入 1MHz）
 * ================================================================ */
#include "bsp_i2s.h"

#include "stm32f4xx_hal.h"

/* ---------------- 引脚（探索者 V3，官方例程确认） ---------------- */
#define I2S_LRCK_GPIO_PORT      GPIOB
#define I2S_LRCK_GPIO_PIN       GPIO_PIN_12      /* WS   AF5 */
#define I2S_SCLK_GPIO_PORT      GPIOB
#define I2S_SCLK_GPIO_PIN       GPIO_PIN_13      /* SCK  AF5 */
#define I2S_SDOUT_GPIO_PORT     GPIOC
#define I2S_SDOUT_GPIO_PIN      GPIO_PIN_2       /* SD   AF6(I2S2ext) */
#define I2S_SDIN_GPIO_PORT      GPIOC
#define I2S_SDIN_GPIO_PIN       GPIO_PIN_3       /* SDIN AF5（录音用，仍配置） */
#define I2S_MCLK_GPIO_PORT      GPIOC
#define I2S_MCLK_GPIO_PIN       GPIO_PIN_6       /* MCK  AF5（原 TIM8_CH1，已让位） */

/* ---------------- I2S2 = SPI2 + TX DMA（DMA1_Stream4/CH0） ---------------- */
#define I2S_SPI                 SPI2
#define I2S_TX_DMA              DMA1_Stream4
#define I2S_TX_DMA_CHANNEL      DMA_CHANNEL_0
#define I2S_TX_DMA_IRQn         DMA1_Stream4_IRQn
#define I2S_TX_DMA_TC_FLAG      DMA_FLAG_TCIF0_4

static I2S_HandleTypeDef  s_i2s;
static DMA_HandleTypeDef  s_txdma;
static void (*s_tx_cb)(void) = NULL;

/* 采样率分频表（VCO 输入 1MHz）：采样率/10, PLLI2SN, PLLI2SR, I2SDIV, ODD */
static const uint16_t I2S_PSC_TBL[][5] = {
    {   800, 256, 5, 12, 1 },   /*  8kHz   */
    {  1102, 429, 4, 19, 0 },   /* 11.025k */
    {  1600, 213, 2, 13, 0 },   /* 16kHz   */
    {  2205, 429, 4,  9, 1 },   /* 22.05k  */
    {  3200, 213, 2,  6, 1 },   /* 32kHz   */
    {  4410, 271, 2,  6, 0 },   /* 44.1k   */
    {  4800, 258, 3,  3, 1 },   /* 48kHz   */
    {  8820, 316, 2,  3, 1 },   /* 88.2k   */
    {  9600, 344, 2,  3, 1 },   /* 96kHz   */
    { 17640, 361, 2,  2, 0 },   /* 176.4k  */
    { 19200, 393, 2,  2, 0 },   /* 192kHz  */
};

uint8_t BSP_I2S_SetSampleRate(uint32_t samplerate)
{
    uint8_t i;
    for (i = 0; i < (uint8_t)(sizeof(I2S_PSC_TBL) / sizeof(I2S_PSC_TBL[0])); i++) {
        if ((samplerate / 10u) == I2S_PSC_TBL[i][0]) {
            break;
        }
    }
    if (i == (uint8_t)(sizeof(I2S_PSC_TBL) / sizeof(I2S_PSC_TBL[0]))) {
        return 1;
    }

    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    clk.PLLI2S.PLLI2SN = I2S_PSC_TBL[i][1];
    clk.PLLI2S.PLLI2SR = I2S_PSC_TBL[i][2];
    HAL_RCCEx_PeriphCLKConfig(&clk);

    RCC->CR |= RCC_CR_PLLI2SON;                 /* 开 PLLI2S */
    while ((RCC->CR & RCC_CR_PLLI2SRDY) == 0u) {
    }

    uint32_t tempreg = I2S_PSC_TBL[i][3];       /* I2SDIV */
    tempreg |= (uint32_t)I2S_PSC_TBL[i][4] << 8; /* ODD */
    tempreg |= 1u << 9;                          /* MCKOE：输出 MCK */
    I2S_SPI->I2SPR = tempreg;
    return 0;
}

void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;

    gpio.Pin = I2S_LRCK_GPIO_PIN;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(I2S_LRCK_GPIO_PORT, &gpio);

    gpio.Pin = I2S_SCLK_GPIO_PIN;
    HAL_GPIO_Init(I2S_SCLK_GPIO_PORT, &gpio);

    gpio.Pin = I2S_SDIN_GPIO_PIN;
    HAL_GPIO_Init(I2S_SDIN_GPIO_PORT, &gpio);

    gpio.Pin = I2S_MCLK_GPIO_PIN;               /* MCK：覆盖原 TIM8_CH1 复用 */
    HAL_GPIO_Init(I2S_MCLK_GPIO_PORT, &gpio);

    gpio.Pin = I2S_SDOUT_GPIO_PIN;
    gpio.Alternate = GPIO_AF6_I2S2ext;           /* SDOUT 走 I2S2ext */
    HAL_GPIO_Init(I2S_SDOUT_GPIO_PORT, &gpio);
}

uint8_t BSP_I2S_Init(uint32_t samplerate)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    s_i2s.Instance = I2S_SPI;
    s_i2s.Init.Mode = I2S_MODE_MASTER_TX;
    s_i2s.Init.Standard = I2S_STANDARD_PHILIPS;
    s_i2s.Init.DataFormat = I2S_DATAFORMAT_16B_EXTENDED;  /* 16bit 扩展帧（例程验证） */
    s_i2s.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
    s_i2s.Init.AudioFreq = samplerate;
    s_i2s.Init.CPOL = I2S_CPOL_LOW;
    s_i2s.Init.ClockSource = I2S_CLOCK_PLL;
    if (HAL_I2S_Init(&s_i2s) != HAL_OK) {
        return 1;
    }

    I2S_SPI->CR2 |= SPI_CR2_TXDMAEN;             /* I2S2 TX DMA 请求使能 */
    __HAL_I2S_ENABLE(&s_i2s);

    /* TX DMA：双缓冲循环 + TC 中断（半缓冲回调填充波形） */
    __HAL_LINKDMA(&s_i2s, hdmatx, s_txdma);
    s_txdma.Instance = I2S_TX_DMA;
    s_txdma.Init.Channel = I2S_TX_DMA_CHANNEL;
    s_txdma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    s_txdma.Init.PeriphInc = DMA_PINC_DISABLE;
    s_txdma.Init.MemInc = DMA_MINC_ENABLE;
    s_txdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_txdma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    s_txdma.Init.Mode = DMA_CIRCULAR;
    s_txdma.Init.Priority = DMA_PRIORITY_HIGH;
    s_txdma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_DeInit(&s_txdma);
    if (HAL_DMA_Init(&s_txdma) != HAL_OK) {
        return 1;
    }

    __HAL_DMA_DISABLE(&s_txdma);
    HAL_NVIC_SetPriority(I2S_TX_DMA_IRQn, 4, 0);   /* ≥5 可 FromISR；音频实时性优先 */
    HAL_NVIC_EnableIRQ(I2S_TX_DMA_IRQn);

    return BSP_I2S_SetSampleRate(samplerate);
}

void BSP_I2S_Play(uint16_t *buf0, uint16_t *buf1, uint16_t num)
{
    /* HAL_DMAEx_MultiBufferStart 成功后将 State=BUSY、Lock=LOCKED 且**不释放**；
     * 若不清零，第二次 Play 会在 __HAL_LOCK/State 检查处直接 HAL_BUSY 返回，
     * DMA 静默不启动（重复播放/连续音效全失效）。故每次启动前显式复位。 */
    s_txdma.State = HAL_DMA_STATE_READY;
    __HAL_UNLOCK(&s_txdma);

    __HAL_DMA_CLEAR_FLAG(&s_txdma, I2S_TX_DMA_TC_FLAG);
    __HAL_DMA_ENABLE_IT(&s_txdma, DMA_IT_TC);
    /* 先使能 TC 中断再启动 DMA：首个缓冲完成事件不被丢失
     * （原顺序 DMA 先跑、中断后开，首 TC 落在窗口外被 CLEAR 丢弃）。 */
    HAL_DMAEx_MultiBufferStart(&s_txdma, (uint32_t)buf0,
                               (uint32_t)&I2S_SPI->DR, (uint32_t)buf1, num);
}

void BSP_I2S_Stop(void)
{
    __HAL_DMA_DISABLE(&s_txdma);
}

void BSP_I2S_SetCallback(void (*cb)(void))
{
    s_tx_cb = cb;
}

uint8_t BSP_I2S_GetCurrentBuf(void)
{
    return ((DMA1_Stream4->CR & DMA_SxCR_CT) != 0u) ? 1u : 0u;
}

/* DMA1_Stream4 中断：传输完成（半缓冲边界）→ 波形填充回调 */
void BSP_I2S_DMA_IRQHandler(void)
{
    if (__HAL_DMA_GET_FLAG(&s_txdma, I2S_TX_DMA_TC_FLAG) != RESET) {
        __HAL_DMA_CLEAR_FLAG(&s_txdma, I2S_TX_DMA_TC_FLAG);
        if (s_tx_cb != NULL) {
            s_tx_cb();
        }
    }
}
