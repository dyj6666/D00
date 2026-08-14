#!/usr/bin/env python3
"""探针：通过 DAP(OpenOCD) AHB-AP 直接操作寄存器，实测板载 W25Q128。

不做任何固件修改：halt -> 喂狗 -> 配置 GPIOB/SPI1 -> 发 0x9F 读 ID -> resume。
目标接线（探索者 V3 原理图）：W25Q128 = SPI1(PB3=SCK/PB4=MISO/PB5=MOSI) + PB14=CS。
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_core import DapSession

# ---- 寄存器地址（STM32F407） ----
RCC_AHB1ENR = 0x40023830   # GPIOB 时钟 = bit1
RCC_APB2ENR = 0x40023844   # SPI1 时钟 = bit12
GPIOB_MODER = 0x40020400
GPIOB_OSPEEDR = 0x40020408
GPIOB_PUPDR = 0x4002040C
GPIOB_ODR = 0x40020414
GPIOB_AFRL = 0x40020420
SPI1_CR1 = 0x40013000
SPI1_SR = 0x40013008
SPI1_DR = 0x4001300C
IWDG_KR = 0x40003000


def main():
    sess = DapSession(clock_khz=500)
    sess.start()

    def rd32(addr, retries=3):
        for _ in range(retries):
            out = sess.cmd("mdw 0x%X 1" % addr)
            m = re.search(r"0x[0-9a-fA-F]+:\s+([0-9a-fA-F]{8})", out)
            if m:
                return int(m.group(1), 16)
            time.sleep(0.02)
        raise RuntimeError("读取失败: 0x%X" % addr)

    def wr32(addr, val):
        sess.cmd("mww 0x%X 0x%X" % (addr, val))

    try:
        sess.halt()
        wr32(IWDG_KR, 0xAAAA)  # 喂狗，防 halt 期间 IWDG 复位

        # 操作前快照（留档：确认 PB3/4/5/14 当前空闲）
        moder0 = rd32(GPIOB_MODER)
        odr0 = rd32(GPIOB_ODR)
        print("PB 快照: MODER=0x%08X ODR=0x%08X" % (moder0, odr0))

        # 1) 使能时钟
        rcc = rd32(RCC_AHB1ENR) | (1 << 1)
        wr32(RCC_AHB1ENR, rcc)
        rcc = rd32(RCC_APB2ENR) | (1 << 12)
        wr32(RCC_APB2ENR, rcc)

        # 2) PB3/PB4/PB5 -> AF 复用；PB14 -> 推挽输出
        m = rd32(GPIOB_MODER)
        m = (m & ~(0x3 << 6))  | (0x2 << 6)   # PB3  AF
        m = (m & ~(0x3 << 8))  | (0x2 << 8)   # PB4  AF
        m = (m & ~(0x3 << 10)) | (0x2 << 10)  # PB5  AF
        m = (m & ~(0x3 << 28)) | (0x1 << 28)  # PB14 Output
        wr32(GPIOB_MODER, m)

        # 3) 高速 + 无上下拉
        o = rd32(GPIOB_OSPEEDR)
        o |= (0x2 << 6) | (0x2 << 8) | (0x2 << 10) | (0x2 << 28)
        wr32(GPIOB_OSPEEDR, o)
        p = rd32(GPIOB_PUPDR)
        p &= ~((0x3 << 6) | (0x3 << 8) | (0x3 << 10) | (0x3 << 28))
        wr32(GPIOB_PUPDR, p)

        # 4) AF5 = SPI1 (PB3/PB4/PB5 在 AFRL 的 bit12..23)
        af = rd32(GPIOB_AFRL)
        af = (af & ~((0xF << 12) | (0xF << 16) | (0xF << 20))) \
             | (0x5 << 12) | (0x5 << 16) | (0x5 << 20)
        wr32(GPIOB_AFRL, af)

        # 5) CS 初始拉高
        wr32(GPIOB_ODR, (odr0 & ~(1 << 14)) | (1 << 14))
        wr32(IWDG_KR, 0xAAAA)

        # 6) SPI1 主模式 8bit，BR=/16(约5.25MHz)，CPOL0/CPHA0，软 NSS
        cr1 = (1 << 2) | (0b100 << 3) | (1 << 6) | (1 << 8) | (1 << 9)
        wr32(SPI1_CR1, 0)
        wr32(SPI1_CR1, cr1)
        time.sleep(0.01)

        def spi_tx_rx(tx):
            wr32(SPI1_DR, tx & 0xFF)
            for _ in range(200):
                sr = rd32(SPI1_SR)
                if sr & 0x01:  # RXNE
                    return rd32(SPI1_DR) & 0xFF
                time.sleep(0.002)
            raise RuntimeError("SPI 超时(RXNE)")

        def spi_wait_idle():
            for _ in range(200):
                sr = rd32(SPI1_SR)
                if not (sr & 0x80):  # BSY 清
                    return
                time.sleep(0.002)
            raise RuntimeError("SPI 忙超时")

        # 7) 发 0x9F 读 JEDEC ID
        odr = rd32(GPIOB_ODR) & ~(1 << 14)   # CS 拉低
        wr32(GPIOB_ODR, odr)
        spi_tx_rx(0x9F)                      # 命令字节（收垃圾）
        spi_wait_idle()
        idb = [spi_tx_rx(0x00) for _ in range(3)]  # 读 3 字节 ID
        spi_wait_idle()
        odr = rd32(GPIOB_ODR) | (1 << 14)    # CS 拉高
        wr32(GPIOB_ODR, odr)

        jedec = (idb[0] << 16) | (idb[1] << 8) | idb[2]
        names = {0xEF4018: "W25Q128 (16MB)", 0xEF4017: "W25Q64 (8MB)",
                 0xEF4016: "W25Q32 (4MB)", 0xEF4015: "W25Q16 (2MB)"}
        print("JEDEC ID = 0x%06X -> %s" % (jedec, names.get(jedec, "未知型号")))
    finally:
        sess.resume()
        sess.close()


if __name__ == "__main__":
    main()
