/* ================================================================
 * cam_link —— OpenART mini 摄像头链路服务（UART5 协议解析）
 *
 * 架构位置：APP 服务层；负责 UART5 接收、帧协议状态机解析、
 *           挥手事件标志与手部/手势状态缓存，供应用层消费。
 * 协议：见 CAMERA/docs/串口协议.md
 *   帧格式 AA 55 | TYPE | LEN | DATA | SUM（TYPE..DATA 累加和低8位）
 *   TYPE 0x01 手部坐标帧 / 0x02 挥手事件 / 0x03 AI 手势帧
 * 数据流：UART5 RXNE 中断（HAL IT 单字节）→ 本模块状态机 →
 *         状态缓存（ISR 写，任务读，volatile）→ GUI/命令消费
 * ================================================================ */
#include "cam_link.h"
#include "usart.h"
#include "bsp_system.h"
#include "event_bus.h"
#include "logger.h"
#include <string.h>

/* ---------------- 协议常量（与 OpenART 侧一致） ---------------- */
#define CAM_FRAME_HEAD1    0xAAu
#define CAM_FRAME_HEAD2    0x55u
#define CAM_TYPE_HAND      0x01u
#define CAM_TYPE_SWIPE     0x02u
#define CAM_TYPE_GESTURE   0x03u
#define CAM_DATA_MAX       16u

/* ---------------- 循环 DMA 接收（顶级：IDLE+DMA，零字节中断） ----------------
 * DMA1_Stream0/CH4 循环模式持续写入 s_rx_dma_buf；
 * UART5 IDLE 中断（一帧结束）→ CamLink_IdleISR 消费缓冲 → 协议状态机。
 * 缓冲 256B >> 帧长 13B，30Hz 下每帧 IDLE 即清空，永不溢出。 */
#define CAM_RX_BUF_SIZE    256u
/* ⚠ DMA 缓冲必须放普通 SRAM（0x20000000）——CCM(0x10000000) 仅 CPU 可访问，
 * DMA 写入无效且触发总线错误（曾致 LCD 卡死）。 */
uint8_t s_rx_dma_buf[CAM_RX_BUF_SIZE]   /* DMA 循环缓冲（cmd_cam 诊断 extern 读） */
    __attribute__((zero_init));
static volatile uint16_t s_rx_rd;   /* 已消费索引（仅 IDLE/满回调 ISR 更新） */

/* ---------------- 接收状态机 ---------------- */
typedef enum {
    CAM_RX_HEAD1,   /* 等待 0xAA */
    CAM_RX_HEAD2,   /* 等待 0x55 */
    CAM_RX_TYPE,
    CAM_RX_LEN,
    CAM_RX_DATA,
    CAM_RX_SUM
} cam_rx_state_t;

/* ---------------- 状态（ISR 写 / 任务读） ---------------- */
static volatile cam_link_state_t s_cam;

static cam_rx_state_t s_rx_state = CAM_RX_HEAD1;
static uint8_t s_rx_type;
static uint8_t s_rx_len;
static uint8_t s_rx_buf[CAM_DATA_MAX];
static uint8_t s_rx_idx;
static uint8_t s_rx_sum;

/* ---------------- 帧校验与分发（ISR 上下文） ---------------- */
static void cam_frame_dispatch(void)
{
    /* 校验：TYPE 到 DATA 末尾累加和（低 8 位） */
    uint8_t sum = s_rx_type;
    for (uint8_t i = 0; i < s_rx_len; i++) {
        sum = (uint8_t)(sum + s_rx_buf[i]);
    }
    if (sum != s_rx_sum) {
        s_cam.err_count++;
        return;
    }

    s_cam.frame_count++;
    s_cam.last_rx_ms = BSP_GetTick();

    switch (s_rx_type) {
    case CAM_TYPE_HAND: {
        /* flags x_lo x_hi y_lo y_hi w_lo w_hi h_lo h_hi（LEN=9） */
        if (s_rx_len < 9u) break;
        s_cam.hand_present = (s_rx_buf[0] & 0x01u) ? 1u : 0u;
        s_cam.hand_x = (uint16_t)(s_rx_buf[1] | ((uint16_t)s_rx_buf[2] << 8));
        s_cam.hand_y = (uint16_t)(s_rx_buf[3] | ((uint16_t)s_rx_buf[4] << 8));
        s_cam.hand_w = (uint16_t)(s_rx_buf[5] | ((uint16_t)s_rx_buf[6] << 8));
        s_cam.hand_h = (uint16_t)(s_rx_buf[7] | ((uint16_t)s_rx_buf[8] << 8));
        break;
    }
    case CAM_TYPE_SWIPE: {
        /* gesture_id（1B）：0x01 LEFT / 0x02 RIGHT / 0x03 UP / 0x04 DOWN */
        if (s_rx_len < 1u) break;
        uint8_t g = s_rx_buf[0];
        if (g == 0x01u) {
            s_cam.swipe_left = 1u;      /* 事件标志：应用层消费后清零 */
        } else if (g == 0x02u) {
            s_cam.swipe_right = 1u;
        }
        s_cam.swipe_count++;
        break;
    }
    case CAM_TYPE_GESTURE: {
        /* gesture_id + confidence（LEN=2） */
        if (s_rx_len < 2u) break;
        s_cam.gesture_id = s_rx_buf[0];
        s_cam.gesture_conf = s_rx_buf[1];
        break;
    }
    default:
        break;
    }
}

/* ---------------- ISR 入口：逐字节喂入状态机 ---------------- */
void CamLink_OnRxByte(uint8_t b)
{
    switch (s_rx_state) {
    case CAM_RX_HEAD1:
        if (b == CAM_FRAME_HEAD1) s_rx_state = CAM_RX_HEAD2;
        break;
    case CAM_RX_HEAD2:
        if (b == CAM_FRAME_HEAD2) {
            s_rx_state = CAM_RX_TYPE;
        } else if (b != CAM_FRAME_HEAD1) {
            s_rx_state = CAM_RX_HEAD1;   /* 失步复位 */
        }
        break;
    case CAM_RX_TYPE:
        s_rx_type = b;
        s_rx_state = CAM_RX_LEN;
        break;
    case CAM_RX_LEN:
        s_rx_len = b;
        s_rx_idx = 0;
        s_rx_sum = 0;
        /* 防呆：非法长度（> 数据区上限）直接失步重同步——否则 s_rx_idx
         * 到 16 后不再增长、永远等不到 SUM，解析器永久卡死（挥手/统计
         * 全失效；噪声/协议错帧可触发）。 */
        if (s_rx_len > CAM_DATA_MAX) {
            s_rx_state = CAM_RX_HEAD1;
        } else {
            s_rx_state = (s_rx_len == 0u) ? CAM_RX_SUM : CAM_RX_DATA;
        }
        break;
    case CAM_RX_DATA:
        if (s_rx_idx < CAM_DATA_MAX) {
            s_rx_buf[s_rx_idx++] = b;
        }
        if (s_rx_idx >= s_rx_len) {
            s_rx_state = CAM_RX_SUM;
        }
        break;
    case CAM_RX_SUM:
        s_rx_sum = b;
        cam_frame_dispatch();
        s_rx_state = CAM_RX_HEAD1;
        break;
    default:
        s_rx_state = CAM_RX_HEAD1;
        break;
    }
}

/* ---------------- 服务接口 ---------------- */
const volatile cam_link_state_t *CamLink_GetState(void)
{
    return &s_cam;
}

const char *CamLink_GestureName(uint8_t id)
{
    switch (id) {
    case 0u:  return "fist";
    case 1u:  return "ok";
    case 2u:  return "one";
    case 3u:  return "palm";
    case 4u:  return "two";
    case 5u:  return "victory";
    case 0xFFu: return "none";
    default:  return "?";
    }
}

/* 消费挥手事件（应用层轮询调用）：返回 1=有挥手，同时清零标志 */
uint8_t CamLink_ConsumeSwipe(uint8_t *dir_out)
{
    uint8_t ev = 0;
    if (s_cam.swipe_left) {
        s_cam.swipe_left = 0;
        if (dir_out) *dir_out = CAM_SWIPE_LEFT;
        ev = 1;
    } else if (s_cam.swipe_right) {
        s_cam.swipe_right = 0;
        if (dir_out) *dir_out = CAM_SWIPE_RIGHT;
        ev = 1;
    }
    return ev;
}

/* 清空事件标志（切页后调用，防止一帧多消费） */
void CamLink_ClearSwipe(void)
{
    s_cam.swipe_left = 0;
    s_cam.swipe_right = 0;
}

/* ---------------- 链路巡检（1s 心跳）：DMA 静默死亡自动恢复 ----------------
 * 背景：HAL_UART_Receive_DMA 会自动使能 PE/ERR 中断；一旦出现帧错误（FE，
 * 线缆接触不良/拔插/悬空噪声）HAL_UART_IRQHandler 错误分支会清除 CR3 DMAR
 * 并 HAL_DMA_Abort——UART5 不在 bsp_uart 表内，无人恢复，链路永久静默死。
 * 巡检：曾收到帧但 last_rx_ms 超时（5s）→ 判定链路死 → 重启 DMA 接收。
 * 拔线场景每 5s 重启一次（开销微秒级），插回后立即恢复，无需复位。 */
#define CAM_SUPERVISE_TIMEOUT_MS  5000u

static uint8_t s_rx_armed;   /* DMA 接收已启动（任务上下文访问） */

static void cam_link_restart_rx(void)
{
    /* 重启期间屏蔽 IDLE 中断：防止 ISR 穿插读取 DMA 计数器的中间态 */
    __HAL_UART_DISABLE_IT(&huart5, UART_IT_IDLE);
    HAL_UART_DMAStop(&huart5);
    huart5.ErrorCode = HAL_UART_ERROR_NONE;
    huart5.RxState = HAL_UART_STATE_READY;
    huart5.RxXferCount = 0;
    s_rx_rd = 0;
    if (HAL_UART_Receive_DMA(&huart5, s_rx_dma_buf, CAM_RX_BUF_SIZE) == HAL_OK) {
        __HAL_UART_ENABLE_IT(&huart5, UART_IT_IDLE);
        s_rx_armed = 1;
        LOG_Printf("[CAM] link DMA restarted (self-heal)\r\n");
    } else {
        s_rx_armed = 0;
        LOG_Printf("[CAM] link DMA restart FAILED\r\n");
    }
}

/* 事件总线 1s 心跳（eventBusTask 上下文） */
static void cam_link_supervise_tick(const message_t *msg)
{
    (void)msg;
    CamLink_Supervise();
}

void CamLink_Supervise(void)
{
    if (!s_rx_armed) {
        return;
    }
    uint32_t now = BSP_GetTick();
    if ((now - s_cam.last_rx_ms) >= CAM_SUPERVISE_TIMEOUT_MS) {
        cam_link_restart_rx();
    }
}

/* ---------------- 初始化：循环 DMA + IDLE 接收 ---------------- */
static void cam_link_drain(void)
{
    /* 从 DMA 环形缓冲取本次到达的字节喂入状态机（仅 ISR 调用） */
    uint16_t cur = CAM_RX_BUF_SIZE - (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart5_rx);
    while (s_rx_rd != cur) {
        CamLink_OnRxByte(s_rx_dma_buf[s_rx_rd]);
        s_rx_rd = (uint16_t)((s_rx_rd + 1u) % CAM_RX_BUF_SIZE);
    }
}

/* UART5 IDLE 中断入口（stm32f4xx_it.c 调用）：一帧结束，消费缓冲 */
void CamLink_IdleISR(void)
{
    if (__HAL_UART_GET_FLAG(&huart5, UART_FLAG_IDLE)) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart5);
        s_cam.idle_count++;
        cam_link_drain();
    }
}

/* HAL 回调：DMA 循环缓冲写满（防御——正常 IDLE 每帧消费，不会满） */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART5) {
        cam_link_drain();
    }
}

void CamLink_Init(void)
{
    memset((void *)&s_cam, 0, sizeof(s_cam));
    s_cam.gesture_id = 0xFFu;   /* 无手势 */
    s_rx_rd = 0;
    s_rx_armed = 0;

    /* 启动循环 DMA 接收 + IDLE 中断（DMA 永不停止） */
    HAL_StatusTypeDef st = HAL_UART_Receive_DMA(&huart5, s_rx_dma_buf, CAM_RX_BUF_SIZE);
    __HAL_UART_ENABLE_IT(&huart5, UART_IT_IDLE);
    s_rx_armed = (st == HAL_OK);
    /* 链路巡检：事件总线 1s 心跳（与 ota_can_svc 同模式，不额外建任务） */
    EventBus_Subscribe(MSG_TICK_1S, cam_link_supervise_tick);
    LOG_Printf("[CAM] link ready (UART5 115200, IDLE+DMA PC12/PD2) dma_rc=%d\r\n",
               (int)st);
}
