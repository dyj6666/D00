/* ================================================================
 * audio_svc —— 音频服务实现
 *
 * 播放机制：
 *   - 采样率 48kHz、16bit、双声道（L=R），I2S 标准 Philips；
 *   - 双缓冲（各 AUDIO_BUF_SAMPLES 样本 = 10ms），DMA 循环 + TC
 *     中断（100Hz）填充"当前 DMA 未读"的缓冲；
 *   - 正弦合成：256 项查表 + 相位累加（freq × 2^32 / 48000 步进），
 *     ISR 内纯查表填充（无锁、无浮点）；
 *   - 缓冲分配自外部 SRAM 池（ExtMem_Alloc），主 RAM 零占用。
 * ================================================================ */
#include "audio_svc.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "queue.h"
#include "task.h"

#include "bsp_es8388.h"
#include "bsp_i2s.h"
#include "ext_mem.h"
#include "logger.h"
#include "wav_data.h"

#define AUDIO_SAMPLE_RATE   48000u
#define AUDIO_BUF_SAMPLES   480u            /* 10ms/缓冲 @48k */
#define AUDIO_QUEUE_LEN     4u

typedef enum {
    AUDIO_SRC_NONE = 0,
    AUDIO_SRC_TONE,
    AUDIO_SRC_WAV
} audio_src_t;

typedef struct {
    audio_src_t   src;
    uint32_t      freq_hz;        /* TONE：频率 */
    uint32_t      duration_ms;    /* TONE：时长 */
    const uint8_t *wav;           /* WAV：数据指针 */
    uint32_t      wav_len;
} audio_req_t;

/* 正弦查表（CCM，512B；Init 预计算） */
static int16_t s_sin_table[256] __attribute__((section(".ccmram"), zero_init));

static QueueHandle_t s_req_q;

static volatile uint8_t s_playing;          /* 播放中标志（ISR 读） */
static volatile uint8_t s_stop_req;         /* 停止请求（ISR 读） */
static uint16_t *s_buf[2];                  /* 双缓冲（ExtMem 池） */
static uint32_t s_phase;                    /* 相位累加器（ISR 更新） */
static uint32_t s_phase_step;               /* 每样本相位步进 */
static uint32_t s_tone_freq;                /* 当前 tone 频率（状态查询） */
static uint8_t  s_volume = 10u;             /* 喇叭音量 0~33 */

/* ---------------- WAV 播放状态（ISR 读；任务在启动播放前设置） ---------------- */
static volatile audio_src_t s_src;          /* 当前播放源 */
static const uint8_t *s_wav_pcm;            /* PCM 游标 */
static uint32_t s_wav_left;                 /* 剩余 PCM 字节 */
static uint32_t s_wav_step;                 /* 每样本字节（channels×bits/8） */
static volatile uint8_t s_wav_done;         /* PCM 播完标志 */

/* ---------------- WAV 解析（标准 PCM 头，健壮性校验） ---------------- */
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    const uint8_t *pcm;
    uint32_t pcm_len;
} wav_info_t;

static int wav_parse(const uint8_t *wav, uint32_t len, wav_info_t *out)
{
    if (wav == NULL || len < 44u) {
        return -1;
    }
    /* RIFF/WAVE 魔数 */
    if (wav[0] != 'R' || wav[1] != 'I' || wav[2] != 'F' || wav[3] != 'F' ||
        wav[8] != 'W' || wav[9] != 'A' || wav[10] != 'V' || wav[11] != 'E') {
        return -1;
    }
    /* fmt 块：PCM 格式（0x0001），16bit 校验（第一版仅 PCM16） */
    if (wav[20] != 1u || wav[21] != 0u) {
        return -1;                           /* 非 PCM */
    }
    uint16_t channels = (uint16_t)(wav[22] | (wav[23] << 8));
    uint32_t rate = (uint32_t)wav[24] | ((uint32_t)wav[25] << 8) |
                    ((uint32_t)wav[26] << 16) | ((uint32_t)wav[27] << 24);
    uint16_t bits = (uint16_t)(wav[34] | (wav[35] << 8));
    if (channels < 1u || channels > 2u || bits != 16u) {
        return -1;                           /* 仅支持 1/2 声道 16bit */
    }
    /* 定位 data 块（支持 fmt 后附加块） */
    uint32_t off = 12u;
    const uint8_t *pcm = NULL;
    uint32_t pcm_len = 0u;
    while (off + 8u <= len) {
        uint32_t blk = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8) |
                       ((uint32_t)wav[off + 6] << 16) | ((uint32_t)wav[off + 7] << 24);
        if (wav[off] == 'd' && wav[off + 1] == 'a' &&
            wav[off + 2] == 't' && wav[off + 3] == 'a') {
            pcm = wav + off + 8u;
            pcm_len = blk;
            break;
        }
        off += 8u + blk;
    }
    if (pcm == NULL || (pcm + pcm_len) > (wav + len)) {
        return -1;
    }
    out->sample_rate = rate;
    out->channels = channels;
    out->bits = bits;
    out->pcm = pcm;
    out->pcm_len = pcm_len;
    return 0;
}

/* ---------------- 波形填充（idx=目标缓冲；ISR 与任务共用） ---------------- */
static void audio_fill(uint32_t idx)
{
    uint16_t *dst = s_buf[idx];
    uint32_t ph = s_phase;
    const int16_t *tbl = s_sin_table;
    for (uint32_t i = 0; i < AUDIO_BUF_SAMPLES; i++) {
        int16_t v = tbl[(ph >> 24u) & 0xFFu];
        dst[i * 2u]      = (uint16_t)v;     /* L */
        dst[i * 2u + 1u] = (uint16_t)v;     /* R */
        ph += s_phase_step;
    }
    s_phase = ph;
}

/* WAV PCM 填充（单声道复制到 L/R；双声道直通；不足静音补尾） */
static void audio_fill_wav(uint32_t idx)
{
    uint16_t *dst = s_buf[idx];
    for (uint32_t i = 0; i < AUDIO_BUF_SAMPLES; i++) {
        if (s_wav_left >= s_wav_step) {
            if (s_wav_step == 2u) {
                int16_t v = (int16_t)(s_wav_pcm[0] | (s_wav_pcm[1] << 8));
                dst[i * 2u] = (uint16_t)v;
                dst[i * 2u + 1u] = (uint16_t)v;
                s_wav_pcm += 2u;
            } else {
                dst[i * 2u] = (uint16_t)(s_wav_pcm[0] | (s_wav_pcm[1] << 8));
                dst[i * 2u + 1u] = (uint16_t)(s_wav_pcm[2] | (s_wav_pcm[3] << 8));
                s_wav_pcm += 4u;
            }
            s_wav_left -= s_wav_step;
        } else {
            dst[i * 2u] = 0u;                /* 尾部静音 */
            dst[i * 2u + 1u] = 0u;
            s_wav_done = 1u;
        }
    }
}

/* ---------------- ISR：DMA TC 回调（填充未读缓冲） ---------------- */
static void audio_tx_cb(void)
{
    if (!s_playing || s_stop_req) {
        return;
    }
    uint32_t idx = BSP_I2S_GetCurrentBuf() ? 0u : 1u;   /* 填 DMA 未读的那块 */
    if (s_src == AUDIO_SRC_WAV) {
        audio_fill_wav(idx);
    } else {
        audio_fill(idx);
    }
}

/* ---------------- 播放任务 ---------------- */
static void audio_task(void *arg)
{
    (void)arg;
    audio_req_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_playing = 1u;
        s_stop_req = 0u;
        s_src = req.src;

        if (req.src == AUDIO_SRC_TONE) {
            if (req.freq_hz == 0u || req.freq_hz > 20000u) {
                s_playing = 0u;
                continue;
            }
            /* 合成参数：相位步进 = freq × 2^32 / 48000 */
            s_phase_step = (uint32_t)(((uint64_t)req.freq_hz << 32u) / AUDIO_SAMPLE_RATE);
            s_phase = 0u;
            s_tone_freq = req.freq_hz;
            audio_fill(0u);                 /* 预填双缓冲 */
            audio_fill(1u);

            BSP_I2S_Play(s_buf[0], s_buf[1], AUDIO_BUF_SAMPLES * 2u);

            if (req.duration_ms == 0u) {
                /* 持续播放直到 Audio_Stop */
                while (!s_stop_req) {
                    vTaskDelay(pdMS_TO_TICKS(10u));
                }
            } else {
                uint32_t left = req.duration_ms;
                while (left > 0u && !s_stop_req) {
                    uint32_t chunk = (left > 10u) ? 10u : left;
                    vTaskDelay(pdMS_TO_TICKS(chunk));
                    left -= chunk;
                }
            }
        } else if (req.src == AUDIO_SRC_WAV) {
            wav_info_t wi;
            if (wav_parse(req.wav, req.wav_len, &wi) != 0) {
                LOG_Printf("[AUD] WAV parse FAILED\r\n");
                s_playing = 0u;
                continue;
            }
            s_wav_pcm = wi.pcm;
            s_wav_left = wi.pcm_len;
            s_wav_step = (uint32_t)wi.channels * (wi.bits / 8u);
            s_wav_done = 0u;

            if (BSP_I2S_SetSampleRate(wi.sample_rate) != 0) {
                LOG_Printf("[AUD] WAV rate %lu unsupported\r\n",
                           (unsigned long)wi.sample_rate);
                s_playing = 0u;
                continue;
            }
            audio_fill_wav(0u);             /* 预填双缓冲 */
            audio_fill_wav(1u);

            BSP_I2S_Play(s_buf[0], s_buf[1], AUDIO_BUF_SAMPLES * 2u);

            while (!s_wav_done && !s_stop_req) {
                vTaskDelay(pdMS_TO_TICKS(10u));
            }
            BSP_I2S_SetSampleRate(AUDIO_SAMPLE_RATE);   /* 恢复默认 48k */
        }

        s_stop_req = 0u;
        s_playing = 0u;
        BSP_I2S_Stop();
    }
}

/* ---------------- 对外接口 ---------------- */

void Audio_PlayTone(uint32_t freq_hz, uint32_t duration_ms)
{
    audio_req_t req;
    req.src = AUDIO_SRC_TONE;
    req.freq_hz = freq_hz;
    req.duration_ms = duration_ms;
    req.wav = NULL;
    req.wav_len = 0u;

    /* 播放中调用 → 替换（清队列旧请求） */
    if (s_playing) {
        s_stop_req = 1u;
        xQueueReset(s_req_q);
    }
    if (xQueueSend(s_req_q, &req, 0u) != pdTRUE) {
        LOG_Printf("[AUD] queue full, tone dropped\r\n");
    }
}

void Audio_PlayWav(const uint8_t *wav_data, uint32_t wav_len)
{
    if (wav_data == NULL || wav_len < 44u) {
        return;
    }
    audio_req_t req;
    req.src = AUDIO_SRC_WAV;
    req.freq_hz = 0u;
    req.duration_ms = 0u;
    req.wav = wav_data;
    req.wav_len = wav_len;

    if (s_playing) {
        s_stop_req = 1u;
        xQueueReset(s_req_q);
    }
    if (xQueueSend(s_req_q, &req, 0u) != pdTRUE) {
        LOG_Printf("[AUD] queue full, wav dropped\r\n");
    }
}

void Audio_Stop(void)
{
    if (s_playing) {
        s_stop_req = 1u;
    }
}

uint8_t Audio_IsPlaying(void)
{
    return s_playing;
}

void Audio_SetVolume(uint8_t volume)
{
    if (volume > 33u) {
        volume = 33u;
    }
    s_volume = volume;
    BSP_ES8388_SpkVolSet(volume);
}

void Audio_GetState(audio_state_t *st)
{
    if (st == NULL) {
        return;
    }
    st->playing = s_playing;
    st->src = (uint8_t)s_src;
    st->freq_hz = s_tone_freq;
    st->volume = s_volume;
}

/* ---------------- 模块初始化（module.c priority=6） ---------------- */
void Audio_Init(void)
{
    /* 正弦表（-3dB 幅度，防削波） */
    for (uint32_t i = 0; i < 256u; i++) {
        s_sin_table[i] = (int16_t)(32767.0f * 0.7f *
                                   sinf((float)i * 2.0f * 3.14159265f / 256.0f));
    }

    /* 波形缓冲：外部 SRAM 池（376KB 统一池，主 RAM 零占用） */
    s_buf[0] = (uint16_t *)ExtMem_Alloc(AUDIO_BUF_SAMPLES * 2u * 2u);
    s_buf[1] = (uint16_t *)ExtMem_Alloc(AUDIO_BUF_SAMPLES * 2u * 2u);
    if (s_buf[0] == NULL || s_buf[1] == NULL) {
        LOG_Printf("[AUD] ExtMem buffer alloc FAILED - audio disabled\r\n");
        return;
    }

    if (BSP_I2S_Init(AUDIO_SAMPLE_RATE) != 0) {
        LOG_Printf("[AUD] I2S init FAILED\r\n");
        return;
    }
    if (BSP_ES8388_Init() != 0) {
        LOG_Printf("[AUD] ES8388 init FAILED\r\n");
        return;
    }

    /* DAC→喇叭通路 + I2S 格式（Philips/16bit）+ 喇叭音量 */
    BSP_ES8388_AddaCfg(1u, 0u);             /* DAC 开、ADC 关 */
    BSP_ES8388_OutputCfg(1u, 1u);           /* 输出通道 1/2（喇叭/耳机） */
    BSP_ES8388_I2sCfg(0u, 3u);              /* Philips + 16bit */
    Audio_SetVolume(s_volume);              /* 喇叭音量（0 最大~33 静音，刻度待实测） */

    BSP_I2S_SetCallback(audio_tx_cb);

    s_req_q = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_req_t));
    /* 任务栈 1KB（audio_task 无大局部变量；wav_parse 仅 8B 结构）。
     * 模块优先级 75（OtaTcp 之后）：启动堆峰值时 OTA/HTTP server alloc
     * 优先成功，音频任务随后创建（见 module.c 注释） */
    osThreadAttr_t attr = {
        .name = "AudioSvc",
        .stack_size = 256 * 4,
        .priority = osPriorityNormal,
    };
    (void)osThreadNew(audio_task, NULL, &attr);

    LOG_Printf("[AUD] ready: I2S2 48kHz + ES8388 -> speaker (buf=ExtMem)\r\n");

    /* 开机旋律（天空之城主题前奏 + 和弦；系统启动完成后响起） */
    Audio_PlayWav(g_wav_startup, g_wav_startup_len);
}
