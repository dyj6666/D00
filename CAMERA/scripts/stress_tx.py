# -*- coding: utf-8 -*-
# stress_tx.py —— UART 满速压测帧流（OpenART mini 侧）
# 用途：将 MCU 端 cam_link 接收链路压到极限——
#   · 连续字节流（帧间无间隙）→ 总线从不空闲 → IDLE 中断不触发
#   · DMA 循环缓冲 256B 写满 → TC 中断 → HAL_UART_RxCpltCallback 消费路径
#   · 频率 ≈ UART 带宽极限（115200bps ≈ 880 帧/s @13B），远超 30Hz 正常流
# 观察：MCU 端 cam 统计（frame_count 应持续增长 / err_count 应接近 0 /
#       系统无死机无卡顿）；停止方法：IDE 停止按钮或复位 OpenART。
import time
from machine import UART

uart = UART(2, baudrate=115200)      # OpenART mini: UART2 = TX:B12
print("STRESS TX ready (UART2 @115200, continuous frames)")

# 0x01 手部帧（协议一致）：AA 55 01 09 | data(9B) | SUM
DATA = bytes([1, 0x60, 0x00, 0x78, 0x00, 0x3C, 0x00, 0x46, 0x00])
s = 0x01
for b in DATA:
    s = (s + b) & 0xFF
FRAME = b'\xAA\x55\x01\x09' + DATA + bytes([s])
# MicroPython v1.18 无 bytes.hex()（OpenART mini = i.MX RT1060 固件）
print("frame size:", len(FRAME), "head:", FRAME[:4])

t0 = time.ticks_ms()
n = 0
while True:
    uart.write(FRAME)                # 阻塞写：等 TX 完成，等效带宽极限
    n += 1
    if n % 2000 == 0:
        t = time.ticks_ms() - t0
        print("sent %d frames in %d ms (~%d fps)" %
              (n, t, n * 1000 // max(t, 1)))
