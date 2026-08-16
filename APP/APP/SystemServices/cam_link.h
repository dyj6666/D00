/* ================================================================
 * cam_link —— OpenART mini 摄像头链路服务接口
 *
 * 架构位置：APP 服务层；与 touch_svc 同级，GUI/命令消费其状态。
 * ================================================================ */
#ifndef CAM_LINK_H
#define CAM_LINK_H

#include <stdint.h>

/* 挥手方向 */
#define CAM_SWIPE_LEFT   0x01u
#define CAM_SWIPE_RIGHT  0x02u
#define CAM_SWIPE_UP     0x03u
#define CAM_SWIPE_DOWN   0x04u

/* 链路状态（ISR 更新，任务只读；挥手事件用消费接口） */
typedef struct {
    uint8_t  hand_present;      /* 手部目标存在 */
    uint16_t hand_x, hand_y;    /* 手中心坐标（QVGA 0~319 / 0~239） */
    uint16_t hand_w, hand_h;    /* 手区域尺寸 */
    uint8_t  gesture_id;        /* AI 手势 0~N-1；0xFF=无 */
    uint8_t  gesture_conf;      /* 置信度 0-100 */
    volatile uint8_t  swipe_left;   /* 挥手事件标志（消费后清零） */
    volatile uint8_t  swipe_right;
    uint32_t frame_count;       /* 有效帧计数 */
    uint32_t err_count;         /* 校验错误计数 */
    uint32_t swipe_count;       /* 挥手事件计数 */
    uint32_t idle_count;        /* IDLE 中断触发计数（调试：区分无数据/DMA 问题） */
    uint32_t last_rx_ms;        /* 最近有效帧时刻 */
} cam_link_state_t;

void CamLink_Init(void);

/* ISR 入口：UART5 单字节接收完成回调调用 */
void CamLink_OnRxByte(uint8_t b);

/* ISR 入口：UART5 IDLE 中断（一帧结束）消费 DMA 环形缓冲 */
void CamLink_IdleISR(void);

/* 查询当前链路状态（ISR 写，volatile；调用方可拷贝快照） */
const volatile cam_link_state_t *CamLink_GetState(void);

/* 消费挥手事件：返回 1=有事件，dir_out 输出方向（CAM_SWIPE_*） */
uint8_t CamLink_ConsumeSwipe(uint8_t *dir_out);

/* 清空挥手标志 */
void CamLink_ClearSwipe(void);

/* 手势 id → 名称（fist/ok/one/palm/two/victory/none/?） */
const char *CamLink_GestureName(uint8_t id);

#endif /* CAM_LINK_H */
