# ================================================================
# bsp —— 板级支持包（ATK-DNESP32S3B / ESP32S3 BOX）
#
# 分层：
#   bus/   共享总线（I2C0 / SPI2），负责互斥、超时、设备挂载
#   chip/  器件驱动，只做"和芯片对话"（寄存器级），不含策略
#   dev/   设备驱动，面向应用的语义 API（灯、按键、屏、卡、音频…）
#   bsp.c  板级入口：按依赖顺序初始化/反初始化
#
# 依赖说明：这里是独立组件，必须显式声明 REQUIRES
# （main 组件则是"不写 REQUIRES 才自动依赖全部"，见本工程 ENGINEERING_LOG.md 第 3 条）
# ================================================================
idf_component_register(
    SRCS
        "src/bsp.c"
        "src/bus/i2c_bus.c"
        "src/bus/spi_bus.c"
        "src/xl9555.c"
        "src/eeprom.c"
        "src/lightsensor.c"
        "src/imu.c"
        "src/touch.c"
        "src/lcd.c"
        "src/audio.c"
        "src/tfcard.c"
        "src/led.c"
        "src/key.c"
        "src/beep.c"
        "src/adc.c"
        "src/temperature.c"
        "src/rtc.c"
        "src/uart.c"
        "src/ui_font.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_timer
        fatfs
        sdmmc
)
