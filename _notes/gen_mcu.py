# -*- coding: utf-8 -*-
"""MCU 交互式知识结构图生成器 v2 —— 全量版（28 子系统 + 14 专题 + 路线 + 资源 = 45 视图）
输出: mcu_structure_interactive.html"""
import json
import html as H
import re as _re

FAM = "Microsoft YaHei, PingFang SC, sans-serif"

SUBS = {}
# __SUBS_A__
SUBS.update({
    "mcu_arch": ("Cortex-M 内核架构", [
        ("工作模式", [("Thread/Handler", "线程模式与异常模式 · MSP/PSP"), ("特权/非特权", "权限分级 · 用户代码通常特权级"), ("寄存器组", "R0-R12 · SP/LR/PC · xPSR")]),
        ("异常模型", [("异常向量表", "0x00000000 起 · 栈顶+复位向量", None, "SCB->VTOR = FLASH_BASE;"), ("异常类型", "Reset/NMI/HardFault/MemManage/Bus/Usage/SVC/PendSV/SysTick"), ("尾链", "Tail-chaining 加速连续异常")]),
        ("存储器映射", [("4GB 地址空间", "0x00000000-0xFFFFFFFF 分区"), ("SRAM/Flash 区", "0x20000000 / 0x08000000"), ("外设区", "0x40000000 起 · 位带区")]),
        ("总线与特性", [("总线矩阵", "I-Bus/D-Bus/S-Bus 并行取指"), ("MPU", "内存保护 · 8 个区域", None, "MPU->RBAR = addr; MPU->RASR = size | AP;"), ("FPU", "单精度浮点 · 硬浮点加速"), ("位带操作", "1 位映射 1 字 · 原子读写"), ("DSP 指令", "SIMD · 饱和运算 · M4 新增")]),
        ("扩展内核", [("M7/M33/M55", "双发射/缓存/TrustZone/Helium"), ("缓存", "I-Cache/D-Cache · 一致性问题"), ("TrustZone", "安全/非安全世界隔离"), ("RISC-V MCU", "CH32V/ESP32C3 · 开源 ISA 新趋势")]),
        ("源码入口", [("CMSIS core_cm4.h", "内核寄存器定义", "https://github.com/ARM-software/CMSIS_5")]),
    ]),
    "boot": ("启动与存储器", [
        ("启动流程", [("BOOT0/BOOT1", "启动源选择(Flash/SRAM/系统存储器)"), ("向量表加载", "SP 初值 + Reset_Handler", None, "ldr r0, =SystemInit; blx r0;"), ("startup 文件", "栈初始化 → 变量清零 → SystemInit → main")]),
        ("Flash 存储", [("主 Flash", "扇区结构 · 擦写寿命 1 万次"), ("选项字节", "读保护 RDP · 写保护 WRPA"), ("IAP 分区", "BOOT/APP 双区 · 跳转"), ("磨损均衡", "日志型存储轮换扇区")]),
        ("SRAM", [("静态内存", "变量/栈/堆 · 掉电丢失"), ("分散加载", "Keil scatter 定义布局", None, "LR_IROM1 0x08000000 0x100000 { ... }"), ("外部存储器", "FMC 扩展 SDRAM/NOR/NAND")]),
        ("数据表示", [("小端序", "STM32 默认小端 · 网络字节序转换", None, "htonl / __REV"), ("对齐", "ARM 对齐要求 · packed 慎用"), ("volatile", "寄存器/中断共享变量必备")]),
        ("启动失败排查", [("向量表错位", "跳转后 HardFault"), ("栈不够", "启动即崩 · 改 Stack_Size"), ("时钟未稳就切", "系统跑飞")]),
        ("源码入口", [("system_stm32f4xx.c", "SystemInit 时钟配置"), ("startup_stm32f4xx.s", "启动汇编模板", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Projects/STM32F4-Discovery/Templates/Src/startup_stm32f4xx.s")]),
    ]),
    "clock": ("时钟系统", [
        ("时钟源", [("HSI/HSE", "内部 16M / 外部晶振", None, "RCC_HSE_ON → 等待就绪"), ("PLL", "倍频主时钟 · 锁相环", None, None, None, "n_clock_pll"), ("LSE/LSI", "低速 32.768K/内部 RC")]),
        ("时钟树", [("SYSCLK", "系统主频(如 168MHz)"), ("AHB/APB 分频", "总线频率 · APB1 上限 42M", None, "RCC_HCLK_Div1, RCC_APB1_Div4"), ("外设时钟使能", "不使能即不工作", None, "__HAL_RCC_GPIOA_CLK_ENABLE();")]),
        ("定时基准", [("SysTick", "1ms 节拍 · HAL_Delay 基础", None, "SysTick_Config(SystemCoreClock / 1000);", None, "n_systick"), ("定时器时钟", "APB1 分频≠1 时定时器 ×2"), ("RTC 时钟", "LSE 日历 · 备份域供电")]),
        ("低功耗模式", [("Sleep", "CPU 停 · 外设活 · 任意中断唤醒"), ("Stop", "全部时钟停 · 唤醒源受限"), ("Standby", "仅备份域 · 复位式唤醒")]),
        ("可靠性", [("看门狗 IWDG", "独立 RC · 喂狗防跑飞", None, "HAL_IWDG_Refresh(&hiwdg);"), ("WWDG", "窗口看门狗 · 精确复位窗口"), ("CSS", "时钟安全检测 · HSE 失效切 HSI")]),
        ("源码入口", [("stm32f4xx_hal_rcc.c", "RCC 全部配置", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c")]),
    ]),
    "nvic": ("中断与异常系统", [
        ("NVIC 基础", [("中断使能", "NVIC_EnableIRQ · 外设中断线"), ("优先级分组", "抢占/子优先级 4+4 位", None, "HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);", None, "n_prio"), ("向量表重定位", "VTOR · IAP 跳转必备")]),
        ("中断服务", [("ISR 编写规范", "快进快出 · 清标志 · 别 printf", None, "void EXTI0_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0); }"), ("HAL 回调链", "IRQHandler → 回调 → 用户代码"), ("嵌套", "高抢占打断低抢占")]),
        ("临界区", [("PRIMASK", "关全局中断 · 短临界", None, "__disable_irq(); ... __enable_irq();", None, "n_critical"), ("BASEPRI", "屏蔽低优先级 · 更精细"), ("RTOS 临界", "taskENTER_CRITICAL 嵌套计数")]),
        ("HardFault 入门", [("常见原因", "野指针/越界/栈溢出/除零"), ("现场取证", "LR 中的 EXC_RETURN · 栈回溯", None, "MRS R0, PSP; 读栈帧", None, "n_hardfault"), ("错误寄存器", "CFSR/HFSR/BFAR/MMFAR")]),
        ("实时性指标", [("中断延迟", "从触发到 ISR 首条指令"), ("嵌套深度", "压栈开销 · 尾链优化"), ("关闭中断时长", "临界区影响中断响应")]),
        ("源码入口", [("core_cm4.h", "NVIC 寄存器定义", "https://github.com/ARM-software/CMSIS_5")]),
    ]),
    "mem_bus": ("存储器与外设总线", [
        ("片上存储", [("Flash", "代码/常量 · 读加速(ART)"), ("SRAM", "变量/栈 · 多块分布"), ("备份域", "RTC 备份寄存器 · VBAT 供电")]),
        ("外部总线", [("FMC/FSMC", "扩展 SRAM/NOR/NAND/SDRAM", None, "FMC_Bank1_NORSRAM_Init(&init);"), ("QSPI", "外部 Flash · XIP 就地执行"), ("内存映射文件", "文件系统跑在外部存储")]),
        ("位带与原子", [("位带区", "SRAM/外设区位操作", None, "#define BITBAND(addr, n) (*(volatile uint32_t *)(0x42000000 + ((uint32_t)(addr)-0x40000000)*32 + (n)*4))"), ("原子访问", "单次读写 · 半字对齐"), ("LDREX/STREX", "独占访问指令")]),
        ("DMA 与缓存", [("Cortex-M7 缓存", "D-Cache 与 DMA 一致性问题", None, "SCB_CleanDCache(); SCB_InvalidateDCache();"), ("MPU 配置", "缓存策略(写回/写通)")]),
        ("源码入口", [("stm32f4xx_hal_fmc.c", "FMC 驱动", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_fmc.c")]),
    ]),
    "gpio": ("GPIO 与外部中断", [
        ("工作模式", [("输入/输出/复用/模拟", "四种模式 · 寄存器 MODER"), ("推挽 vs 开漏", "输出能力 · 电平转换/线与", None, "GPIO_MODE_OUTPUT_PP / GPIO_MODE_OUTPUT_OD", None, "n_gpio_mode"), ("上下拉", "浮空/上拉/下拉 · 按键必配")]),
        ("操作 API", [("写引脚", "HAL_GPIO_WritePin/TogglePin", None, "HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, SET);"), ("读引脚", "HAL_GPIO_ReadPin"), ("复用功能", "AF 表 · UART/TIM 引脚选择")]),
        ("EXTI 中断", [("外部中断线", "16 线 PA0-PG15 共享", None, "HAL_GPIO_EXTI_Callback(GPIO_PIN_0);", None, "n_exti"), ("触发方式", "上升/下降/双边沿"), ("回调机制", "HAL 中断回调 · 勿放耗时逻辑")]),
        ("电气特性", [("灌电流/拉电流", "sink/source 能力 · 限流电阻"), ("施密特触发", "输入迟滞 · 抗抖动"), ("输出速度", "GPIO 速度等级 · EMC 权衡")]),
        ("驱动设计", [("按键消抖", "延时/定时器扫描 · 状态机"), ("LED 驱动", "低电平点亮 · 限流电阻"), ("开漏应用", "I2C 线与 · 电平转换"), ("多路复用", "引脚冲突排查流程")]),
        ("源码入口", [("stm32f4xx_hal_gpio.c", "HAL GPIO 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c")]),
    ]),
    "timer": ("定时器", [
        ("定时器家族", [("基本/通用/高级", "TIM6/7 · TIM2-5 · TIM1/8"), ("时基单元", "PSC 预分频 + ARR 自动重装", None, "__HAL_TIM_SET_AUTORELOAD(&htim, 999);"), ("计数模式", "向上/向下/中心对齐")]),
        ("PWM 输出", [("PWM 模式", "CCR 比较 · 占空比", None, "__HAL_TIM_SET_COMPARE(&htim, TIM_CHANNEL_1, 500);", None, "n_pwm"), ("频率与占空比", "f = 时钟/(PSC+1)/(ARR+1)"), ("死区/互补", "高级定时器 · 电机桥臂保护")]),
        ("输入捕获", [("捕获原理", "边沿触发 · 记录 CNT", None, "HAL_TIM_IC_CaptureCallback(&htim);"), ("测频率/脉宽", "两次捕获差值"), ("编码器模式", "正交解码 · 转速/方向"), ("PWM 输入", "双通道测周期+占空比")]),
        ("定时器联动", [("级联", "主从定时器同步"), ("DMA 触发", "定时器驱动 DMA 搬运", None, "__HAL_TIM_ENABLE_DMA(&htim, TIM_DMA_CC1);"), ("触发 ADC", "精确采样时刻")]),
        ("应用场景", [("系统时基", "调度/延时/超时检测"), ("电机控制", "PWM+捕获闭环"), ("脉冲计数", "流量计/编码器")]),
        ("源码入口", [("stm32f4xx_hal_tim.c", "HAL TIM 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c")]),
    ]),
    "pwm_motor": ("电机控制与 PWM 驱动", [
        ("电机类型", [("直流有刷", "H 桥 · 方向+调速", None, "HAL_GPIO_WritePin(IN1, SET); __HAL_TIM_SET_COMPARE(&htim, CH1, duty);"), ("无刷 BLDC", "六步换相 · 反电动势检测"), ("步进电机", "脉冲+方向 · 细分"), ("舵机", "20ms 周期 · 1-2ms 脉宽")]),
        ("驱动电路", [("H 桥", "NMOS 全桥 · 续流二极管"), ("驱动芯片", "TB6612/L298N/DRV8871"), ("光耦隔离", "功率地与逻辑地隔离")]),
        ("控制算法", [("PID 基础", "P/I/D 作用 · 调参方法", None, "out = Kp*e + Ki*Σe + Kd*(e-e_prev);"), ("速度环/位置环", "级联控制"), ("PWM 频率选择", "20kHz 超声 · 电机噪音")]),
        ("高级电机", [("FOC 矢量控制", "Clarke/Park 变换 · 电流环"), ("霍尔传感器", "换相位置反馈"), ("堵转保护", "电流检测 · 过流断开")]),
        ("源码入口", [("STM32 电机控制 SDK", "MCSDK 官方", "https://github.com/STMicroelectronics/motor-control-sdk")]),
    ]),
    "dma": ("DMA 与外设联动", [
        ("DMA 基础", [("通道/流", "8 流 × 8 通道 · 优先级"), ("传输模式", "单次/循环/双缓冲", None, "HAL_UART_Receive_DMA(&huart, buf, N);"), ("方向", "外设→内存 · 内存→外设 · 内存→内存")]),
        ("与外设联动", [("UART 收发", "空闲中断+DMA 不定长接收", None, None, None, "n_dma"), ("ADC 采集", "多通道连续 → 内存数组", None, "HAL_ADC_Start_DMA(&hadc, buf, N);"), ("TIM 触发", "定时器触发 ADC 采样"), ("SPI 传输", "大块数据零 CPU")]),
        ("工程要点", [("缓冲对齐", "4 字节对齐 · 半字/字传输"), ("数据一致性", "DMA 完成后处理 · 防撕裂"), ("双缓冲", "乒乓切换 · 无缝采集"), ("与中断对比", "批量 vs 逐次 · 中断频率")]),
        ("源码入口", [("stm32f4xx_hal_dma.c", "HAL DMA 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c")]),
    ]),
    "adc": ("ADC/DAC 模拟外设", [
        ("ADC 原理", [("逐次逼近", "SAR 结构 · 12 位分辨率", None, "HAL_ADC_Start(&hadc); HAL_ADC_PollForConversion(&hadc, 10);", None, "n_adc"), ("采样时间", "通道采样周期 · 内阻影响"), ("参考电压", "VREF · 满量程基准")]),
        ("采集模式", [("单次/连续", "一次 vs 循环转换"), ("扫描多通道", "序列通道自动轮询", None, "HAL_ADC_Start_DMA(&hadc, buf, 8);"), ("注入通道", "高优先级抢占"), ("多 ADC 同步", "并行采样 · 定时器触发")]),
        ("数据处理", [("电压换算", "adc × Vref / 4095", None, "v = (float)adc * 3.3f / 4095.0f;"), ("滤波", "均值/中值/滑动平均"), ("校准", "失调/增益补偿 · 内部基准")]),
        ("进阶", [("过采样", "分辨率提升 · 噪声整形"), ("输入阻抗", "RC 匹配 · 采样保持电容"), ("内部传感器", "温度/VBAT 通道")]),
        ("DAC", [("数模输出", "8/12 位 · 波形发生"), ("DMA 播放", "采样数据循环输出", None, "HAL_DAC_Start_DMA(&hdac, ch, buf, n, DAC_ALIGN_12B_R);")]),
        ("源码入口", [("stm32f4xx_hal_adc.c", "HAL ADC 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c")]),
    ]),
    "usb": ("USB 接口", [
        ("协议基础", [("拓扑", "主机/集线器/设备 · 端点"), ("传输类型", "控制/批量/中断/同步", None, "USB_EP_TYPE_BULK / USB_EP_TYPE_INT"), ("枚举过程", "复位→地址→描述符→配置")]),
        ("描述符", [("设备/配置/接口/端点", "四级描述符结构"), ("CDC 类", "虚拟串口 · 最常用", None, "USBD_CDC_HandleTypeDef hUsbDeviceCDC;"), ("HID 类", "键盘鼠标 · 免驱"), ("MSC 类", "U 盘 · 大容量存储")]),
        ("模式", [("设备/主机/OTG", "三种模式选择"), ("供电", "自供电/总线供电 · 500mA"), ("速度", "FS 12M / HS 480M")]),
        ("工程要点", [("电源枚举失败", "先查 D+ 上拉与 VBUS"), ("描述符错误", "枚举卡在 get_descriptor"), ("热插拔", "检测与去抖")]),
        ("源码入口", [("STM32 USB 中间件", "USBD 库", "https://github.com/STMicroelectronics/STM32CubeF4/tree/master/Middlewares/ST/STM32_USB_Device_Library")]),
    ]),
    "eth": ("以太网与网络", [
        ("硬件架构", [("MAC+PHY", "内嵌 MAC · 外置 PHY(LAN8720)", None, "HAL_ETH_Init(&heth);"), ("RMII/MII", "精简/标准接口 · 时钟 50M"), ("PHY 地址", "配置引脚 · 寄存器访问")]),
        ("协议栈", [("LwIP", "轻量 TCP/IP", None, "tcp_new(); tcp_bind(pcb, IP_ADDR_ANY, 80);"), ("Socket API", "netconn/socket 层"), ("内存管理", "PBUF 池 · 零拷贝")]),
        ("应用", [("HTTP/WebSocket", "设备网页配置"), ("MQTT", "云端接入", None, "mqtt_connect(&c, &ci);"), ("TCP/UDP", "流式 vs 数据报"), ("以太网 DMA", "描述符环 · 中断收包")]),
        ("调试", [("PHY 寄存器", "链路/协商状态", None, "HAL_ETH_ReadPHYRegister(&heth, PHY_BSR, &val);"), ("抓包", "Wireshark 镜像口/旁路"), ("丢包排查", "缓冲不足/CRC/冲突")]),
        ("源码入口", [("LwIP 官方", "轻量协议栈", "https://savannah.nongnu.org/projects/lwip/")]),
    ]),
})
# __SUBS_B__
# __SUBS_B__
SUBS.update({
    "uart": ("串行通信 UART/RS485", [
        ("UART 基础", [("帧格式", "起始位+数据+校验+停止", None, "HAL_UART_Transmit(&huart1, buf, len, 100);"), ("波特率", "过采样 · 误差预算 <2%"), ("收发模式", "轮询/中断/DMA 三态")]),
        ("UART 进阶", [("环形缓冲", "中断逐字节入环 · 主循环消费", None, "ringbuf_put(&rb, byte);"), ("空闲中断+DMA", "不定长接收的黄金方案", None, None, None, "n_uart_dma"), ("流控", "RTS/CTS 硬件流控"), ("丢数据根因", "未及时取走 · 缓冲过小")]),
        ("RS485", [("半双工", "方向控制 · 收发切换时序", None, "DE=1; HAL_UART_Transmit(...); DE=0;"), ("终端电阻", "120Ω 匹配 · 菊花链"), ("Modbus RTU", "工业标准协议 · CRC16", None, "crc = crc16_modbus(buf, len);")]),
        ("RS232", [("电平转换", "±12V 电平 · MAX3232"), ("DB9 引脚", "TXD/RXD/GND 三线即可")]),
        ("协议设计", [("帧格式", "帧头+长度+载荷+校验"), ("粘包/半包", "状态机解析 · 超时判帧"), ("重传机制", "ACK/超时/序列号")]),
        ("源码入口", [("stm32f4xx_hal_uart.c", "HAL UART", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c")]),
    ]),
    "spi": ("SPI 总线", [
        ("协议基础", [("4 线制", "SCK/MOSI/MISO/CS · 全双工", None, "HAL_SPI_Transmit(&hspi, buf, n, 100);"), ("4 种模式", "CPOL/CPHA 时序组合"), ("波特率", "分频 · 上限 42MHz")]),
        ("数据操作", [("读写一体", "全双工 · 发多少收多少", None, "HAL_SPI_TransmitReceive(&hspi, tx, rx, n, 100);"), ("读操作", "先发命令+地址再收数据"), ("CS 管理", "片选时序 · 多从机")]),
        ("典型器件", [("SPI Flash", "W25Q 系列 · 读/写/擦除", None, "W25Q_ReadData(addr, buf, len);"), ("OLED", "SSD1306 · 命令/数据切换"), ("传感器", "加速度计/陀螺仪寄存器访问")]),
        ("进阶", [("QSPI", "四线数据 · 高吞吐 · XIP"), ("DMA+SPI", "大块传输零 CPU", None, "HAL_SPI_Transmit_DMA(&hspi, buf, n);"), ("菊花链", "多器件串联"), ("从机模式", "外部主机控制")]),
        ("调试", [("示波器看时序", "CPOL/CPHA 不匹配典型症状"), ("MISO 无数据", "从机未选通/未就绪"), ("噪声", "走线/地线/上拉")]),
        ("源码入口", [("stm32f4xx_hal_spi.c", "HAL SPI", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c")]),
    ]),
    "i2c": ("I2C 总线", [
        ("协议基础", [("两线制", "SCL/SDA · 开漏+上拉", None, "HAL_I2C_Mem_Read(&hi2c1, addr, reg, 1, buf, n, 100);", None, "n_i2c"), ("地址与 ACK", "7/10 位地址 · 应答机制"), ("时序", "起始/停止/字节/ACK 位")]),
        ("多主机", [("线与仲裁", "天然多主机 · 冲突检测"), ("时钟拉伸", "从机慢速拉低 SCL"), ("SMBus", "超时机制 · 系统管理总线")]),
        ("典型器件", [("EEPROM", "AT24C 系列 · 页写", None, "HAL_I2C_Mem_Write(&hi2c1, 0xA0, addr, 2, data, n, 100);"), ("温湿度", "SHT30/AHT20"), ("触摸/显示", "OLED/触摸屏")]),
        ("工程要点", [("总线卡死", "SDA 被拉低 · 时钟恢复", None, "// 9 个时钟脉冲复位从机"), ("速率", "100K/400K/1M · 上拉阻值"), ("死锁恢复", "GPIO 模拟时钟脉冲")]),
        ("源码入口", [("stm32f4xx_hal_i2c.c", "HAL I2C", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c")]),
    ]),
    "can": ("CAN 总线", [
        ("协议基础", [("CAN2.0A/B", "标准帧 11 位 ID · 扩展帧 29 位"), ("帧类型", "数据帧/远程帧/错误帧/过载帧"), ("仲裁机制", "ID 越小优先级越高 · 无损仲裁", None, "CAN_ID_STD | CAN_RTR_DATA", None, "n_can")]),
        ("位时序", [("波特率计算", "BRP+段配置 · 采样点 75%-85%"), ("同步", "硬同步/重同步 · SJW"), ("总线长度", "与速率的关系 · 1Mbps≈40m")]),
        ("滤波器", [("过滤器组", "28 组 · 标识符掩码/列表模式", None, "HAL_CAN_ConfigFilter(&hcan, &sFilterConfig);"), ("接收匹配", "只收关心的 ID"), ("FIFO", "两个接收 FIFO · 溢出管理")]),
        ("错误处理", [("错误状态", "主动/被动/总线关闭"), ("错误计数", "TEC/REC · 恢复策略", None, "if (hcan.ErrorCode & HAL_CAN_ERROR_BOF) { 复位重启; }"), ("收发器", "TJA1050 · 终端电阻")]),
        ("CAN FD", [("可变速率", "数据段 5Mbps · 更高效载荷"), ("兼容性", "与经典 CAN 混合组网"), ("应用", "车载/工业总线")]),
        ("应用协议", [("CANopen", "对象字典 · PDO/SDO"), ("UDS", "诊断服务 · 0x22/0x2E"), ("私有协议", "ID 规划 · DBC 文件")]),
        ("源码入口", [("stm32f4xx_hal_can.c", "HAL CAN 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_can.c")]),
    ]),
    "rtos": ("FreeRTOS 内核", [
        ("任务", [("任务创建", "xTaskCreate · 栈分配", None, "xTaskCreate(vTask1, \"t1\", 128, NULL, 2, NULL);", None, "n_freertos_task"), ("任务状态", "就绪/运行/阻塞/挂起"), ("任务删除", "自杀/他杀 · 资源释放")]),
        ("调度", [("优先级抢占", "高优先级就绪即抢占", None, "vTaskDelay(pdMS_TO_TICKS(100));"), ("时间片", "同优先级轮转 · configTICK_RATE_HZ"), ("空闲任务", "钩子函数 · 低功耗入口")]),
        ("任务通信", [("队列", "消息传递 · 生产者消费者", None, "xQueueSend(q, &val, 0); xQueueReceive(q, &val, portMAX_DELAY);", None, "n_queue"), ("信号量", "二值/计数 · 事件通知"), ("互斥量", "优先级继承 · 防反转", None, None, None, "n_mutex"), ("事件组/任务通知", "多事件同步 · 轻量替代")]),
        ("内存管理", [("heap_1~5", "静态/动态/合并/分页/线程安全", None, None, None, "n_heap"), ("栈溢出检测", "hook 函数 · 栈高水位")]),
        ("中断集成", [("ISR 安全 API", "FromISR 后缀 · xHigherPriorityTaskWoken", None, "portYIELD_FROM_ISR(xHigherPriorityTaskWoken);"), ("延迟中断处理", "中断只发信号 · 任务干活")]),
        ("源码入口", [("tasks.c", "调度核心", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/tasks.c"), ("queue.c", "队列/信号量实现", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/queue.c")]),
    ]),
    "rtos_adv": ("RTOS 进阶与实时性", [
        ("调度深入", [("tickless 低功耗", "停止 tick · 定时器唤醒", None, "configUSE_TICKLESS_IDLE 1"), ("SMP 多核", "FreeRTOS SMP · 任务亲和性"), ("调度延迟", "中断关闭时间的影响")]),
        ("高级机制", [("软件定时器", "守护任务实现 · 精度", None, "xTimerCreate(\"t\", 1000, pdTRUE, NULL, cb);"), ("流缓冲/消息缓冲", "无锁环形 · 任意大小消息"), ("任务通知", "比信号量快 45% · 轻量")]),
        ("内存与栈", [("静态分配", "编译期确定 · 安全关键必用", None, "StaticTask_t xTaskBuffer; xTaskCreateStatic(...);"), ("栈高水位", "uxTaskGetStackHighWaterMark"), ("内存碎片", "heap_4 合并 · 规划分配")]),
        ("实时性设计", [("中断延迟预算", "ISR 必须短 · 最坏情况分析"), ("优先级分配", "速率单调/关键度排序"), ("优先级反转", "互斥量 vs 信号量", None, "// 低优持锁→高优等待→继承"), ("死锁预防", "锁序一致 · 超时获取")]),
        ("裸机 vs RTOS", [("前后台系统", "主循环+中断 · 简单场景"), ("何时上 RTOS", "多任务/实时约束/复杂状态"), ("事件驱动", "超级循环 + 状态机")]),
        ("源码入口", [("FreeRTOS 官方文档(中文)", "权威参考", "https://www.freertos.org/Documentation/")]),
    ]),
    "lowpower": ("低功耗与可靠性", [
        ("低功耗设计", [("模式选择", "Sleep/Stop/Standby 权衡"), ("功耗预算", "μA 级指标 · 唤醒占比"), ("唤醒源", "RTC/EXTI/看门狗"), ("tickless", "RTOS 配合低功耗")]),
        ("可靠性", [("看门狗策略", "IWDG 兜底 · 任务级喂狗", None, "HAL_IWDG_Refresh(&hiwdg);", None, "n_wdt"), ("掉电保存", "RTC 备份寄存器/Flash"), ("CRC 校验", "数据完整性", None, "HAL_CRC_Calculate(&hcrc, buf, len);")]),
        ("固件升级", [("IAP 设计", "BOOT/APP 双区 · 跳转表", None, "((void (*)(void))APP_ADDR)();", None, "n_iap"), ("OTA 通道", "串口/网络/CAN 下载"), ("升级安全", "校验失败回滚 · 断电保护")]),
        ("量产", [("读保护", "RDP 防抄板 · 提防误锁", None, "FLASH_OB_RDP_LevelConfig(OB_RDP_LEVEL_1);"), ("唯一 ID", "UID 序列化/加密"), ("测试覆盖", "边界电压/温度")]),
        ("源码入口", [("stm32f4xx_hal_flash.c", "Flash 编程/选项字节", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c")]),
    ]),
    "memory_mgmt": ("内存管理与数据结构", [
        ("内存模型", [("栈/堆/全局", "三种存储区 · 大小规划"), ("堆管理", "malloc 碎片 · 内核风格内存池", None, "// 固定大小内存池"), ("内存对齐", "4/8 字节对齐 · 缓存行")]),
        ("数据结构", [("环形缓冲", "生产者消费者 · 无锁单对单", None, "struct ringbuf { uint8_t *buf; uint16_t head, tail; };"), ("链表", "嵌入式链表 · 内存池节点"), ("队列/栈", "任务通信与深度优先"), ("查找表", "查表替代计算 · 空间换时间")]),
        ("工程模式", [("内存池", "预分配 · 无碎片 · 实时安全", None, "pool_alloc(&pool); pool_free(&pool, p);"), ("零拷贝", "指针传递 · 引用计数"), ("DMA 缓冲", "对齐分配 · 双缓冲")]),
        ("常见问题", [("栈溢出", "深递归/大数组 · 金丝雀", None, "// 检查栈填充模式"), ("内存泄漏", "分配未释放 · 计数调试"), ("越界写", "相邻对象被破坏 · 金丝雀")]),
        ("源码入口", [("FreeRTOS heap_4", "参考实现", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/MemMang/heap_4.c")]),
    ]),
    "software_arch": ("软件架构与设计模式", [
        ("分层架构", [("BSP/驱动/中间件/应用", "四层职责分离", None, "app → service → hal → bsp"), ("接口设计", "头文件即契约 · 依赖倒置"), ("模块化", "高内聚低耦合 · 静态注册表")]),
        ("状态机", [("有限状态机", "事件驱动 · 转移表", None, "state = next[state][event];"), ("层次状态机", "嵌套状态 · 复用"), ("防抖/协议解析", "状态机经典应用")]),
        ("事件驱动", [("事件队列", "ISR 发事件 · 主循环处理", None, "evtq_post(EVT_KEY);"), ("发布订阅", "解耦生产消费者"), ("回调模式", "HAL 回调 · 上下文指针")]),
        ("实时模式", [("时间片轮询", "超循环调度表", None, "task_table[i].period 检查"), ("协程/任务", "RTOS 任务划分原则"), ("看门狗任务", "任务级健康监测")]),
        ("代码质量", [("断言", "参数校验 · 不可达路径", None, "configASSERT(x);"), ("日志分级", "调试/信息/错误 · 环形缓冲"), ("错误码约定", "统一返回码 · 集中处理")]),
        ("源码入口", [("嵌入式架构参考", "开源嵌入式项目", "https://github.com/topics/embedded-systems")]),
    ]),
})
# __SUBS_C__
SUBS.update({
    "toolchain": ("工具链与构建", [
        ("构建系统", [("Keil uVision", "uvprojx · 工程配置", None, "UV4 -b project.uvprojx -o build.log"), ("GCC/CMake", "跨平台 · CI 友好", None, "arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -O2"), ("链接脚本", "FLASH/SRAM 布局 · 符号导出")]),
        ("烧录与调试", [("SWD/JTAG", "两线 vs 四线 · 速度"), ("调试器", "ST-Link/J-Link/DAP", None, "openocd -f interface/stlink.cfg -f target/stm32f4x.cfg"), ("下载算法", "Flash 编程 · 校验")]),
        ("代码生成", [("STM32CubeMX", "图形化配置 · 代码生成", None, "// MX_GPIO_Init(); MX_USART1_UART_Init();"), ("HAL vs LL", "易用 vs 轻量 · 混合使用"), ("库选择", "HAL/LL/寄存器三选")]),
        ("工程组织", [("目录结构", "Core/Drivers/Middleware/App 分层"), ("版本管理", "git · 固件版本号规范"), ("CI/CD", "GitHub Actions 自动构建", None, "actions/checkout + make")]),
        ("源码入口", [("OpenOCD", "开源调试烧录", "https://openocd.org/"), ("STM32CubeProgrammer", "官方烧录工具")]),
    ]),
    "debug_adv": ("调试技术与故障定位", [
        ("内核级调试", [("ITM/SWO", "指令跟踪 · 1 引脚输出日志", None, "ITM_SendChar(c);"), ("DWT", "数据观察点 · 性能计数器", None, "DWT->CYCCNT"), ("ETM/ETB", "全指令跟踪")]),
        ("RTOS 调试", [("任务视图", "RTOS 插件看任务状态", None, "// 查看 栈水位/优先级/状态"), ("调度器感知", "调试器识别 TCB"), ("临界区卡死", "中断关死/死锁排查")]),
        ("故障注入", [("看门狗触发", "验证复位路径"), ("电压跌落", "brown-out 行为"), ("干扰测试", "ESD/EFT 复位恢复")]),
        ("性能分析", [("函数耗时", "GPIO 翻转+示波器", None, "GPIOE->ODR ^= 1<<1; // 测量区间"), ("中断频率", "计数器统计"), ("CPU 占用", "空闲任务计数器")]),
        ("工具组合", [("RTT", "SEGGER 实时传输 · 双向往返"), ("Semihosting", "半主机 printf"), ("逻辑分析仪", "协议时序抓取", None, "// Saleae/PulseView")]),
        ("源码入口", [("SEGGER RTT", "实时调试", "https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/")]),
    ]),
    "reliability": ("可靠性与 EMC", [
        ("硬件防护", [("ESD 保护", "TVS 管 · 接口防护", None, "// 串口/USB 加 TVS"), ("电源防护", "反接/过压/浪涌"), ("去耦设计", "100nF 每 IC · 大电容储能")]),
        ("EMC 基础", [("EMI/EMS", "辐射发射与抗扰度"), ("地平面", "完整地 · 最小回路", None, "// 四层板信号完整性"), ("滤波", "磁珠/共模电感/π 滤波")]),
        ("软件健壮性", [("看门狗体系", "主循环+任务级双保险"), ("错误恢复", "异常复位 · 状态保存", None, "// 复位原因寄存器判断"), ("参数校验", "外部输入全校验")]),
        ("安全认证", [("IEC 61508", "功能安全 · SIL 等级"), ("MISRA C", "编码规范 · 静态分析", None, "// cppcheck --enable=all"), ("测试覆盖率", "语句/分支/MC/DC")]),
        ("生产测试", [("ICT/烧录/老化", "三阶段测试"), ("边界测试", "电压/温度/频率扫描"), ("可追溯性", "序列号+测试记录")]),
        ("源码入口", [("MISRA C 指南", "编码规范", "https://www.misra.org.uk/")]),
    ]),
    "security": ("嵌入式安全", [
        ("基础安全", [("加密算法", "AES/CRC/真随机数", None, "HAL_CRYP_AES_Encrypt(&hcryp, in, out, 1000);"), ("RNG 随机数", "硬件熵源", None, "HAL_RNG_GenerateRandomNumber(&hrng, &val);"), ("哈希", "SHA 完整性校验")]),
        ("固件保护", [("读保护 RDP", "防读 Flash · 级别选择"), ("安全启动", "签名校验 · 信任根", None, "// 公钥验证固件签名"), ("固件加密", "下载链路加密 · 防抓包")]),
        ("TrustZone", [("安全/非安全", "M23/M33/M55 硬件隔离"), ("安全外设", "密钥存储 · 隔离访问")]),
        ("防抄板", [("唯一 ID", "UID 绑定 · 加密算法", None, "// 用 UID 派生密钥"), ("混淆", "非对称成本 · 别过度"), ("安全存储", "备份寄存器/OTP")]),
        ("通信安全", [("TLS 最小化", "mbedTLS · 资源受限", None, "mbedtls_ssl_handshake(&ssl);"), ("密钥管理", "出厂预置/安全烧录")]),
        ("源码入口", [("mbedTLS", "轻量加密库", "https://github.com/Mbed-TLS/mbedtls")]),
    ]),
    "electronics": ("电子电路基础", [
        ("电平与接口", [("TTL/CMOS", "电平标准 · 3.3/5V 互转"), ("上拉/下拉", "阻值计算 · 电流预算", None, "R = (Vcc - Vih) / Ipu"), ("开漏/推挽", "线与逻辑 · 驱动能力")]),
        ("基本电路", [("三极管/MOSFET", "开关电路 · 驱动负载", None, "// NPN 低边开关"), ("分压/限流", "LED 限流电阻计算", None, "R = (3.3 - 2.0) / 0.01"), ("二极管", "续流/防反接/钳位")]),
        ("电源", [("LDO vs DCDC", "噪声 vs 效率", None, "// 3.3V LDO 或 Buck"), ("电池供电", "放电曲线 · 电量检测"), ("电源时序", "多路供电上电顺序")]),
        ("信号调理", [("RC 滤波", "去抖/低通", None, "// τ = R*C 选择"), ("运放", "跟随/放大/比较"), ("光耦", "隔离 · 电流传输比")]),
        ("测量", [("万用表", "电压/电流/通断"), ("示波器", "波形/时序/毛刺"), ("可调电源", "限流保护 · 上电观察")]),
    ]),
    "sensors": ("传感器与显示", [
        ("环境传感", [("温湿度", "DHT11/SHT30 · 时序协议", None, "// DHT 单总线时序"), ("气压", "BMP280 · I2C"), ("光照/气体", "光敏/空气质量")]),
        ("运动传感", [("IMU", "MPU6050 · 加速度+陀螺仪", None, "HAL_I2C_Mem_Read(&hi2c, 0x68<<1, 0x3B, 1, acc, 6, 100);"), ("磁力计", "HMC5883L · 航向"), ("融合算法", "互补/卡尔曼滤波")]),
        ("定位与通信", [("GPS/北斗", "NMEA 协议解析", None, "// $GPRMC 解析"), ("RF", "NRF24L01/LoRa/蓝牙")]),
        ("显示", [("OLED", "SSD1306 · I2C/SPI 驱动", None, "OLED_ShowString(0, 0, \"HELLO\");"), ("LCD/TFT", "ST7735/ILI9341 · 取模"), ("段码/LED 点阵", "动态扫描 · 消隐")]),
        ("人机交互", [("按键矩阵", "扫描+消抖", None, "// 行列扫描"), ("触摸", "电容触摸 · 手势"), ("编码器旋钮", "正交信号")]),
        ("源码入口", [("传感器驱动合集", "开源社区", "https://github.com/STMicroelectronics/stm32-ai-model-zoo")]),
    ]),
    "pcb": ("PCB 与硬件联调", [
        ("原理图", [("引脚核对", "复用功能表 · 冲突检查", None, "// AF 表核对"), ("电源树", "各级电压/电流预算"), ("器件选型", "封装/功耗/供货")]),
        ("PCB 布局", [("去耦电容", "靠近电源引脚"), ("晶振布局", "负载电容 · 走线短", None, "// CL = (C1*C2)/(C1+C2) + Cs"), ("地平面", "完整地 · 过孔回流")]),
        ("信号完整性", [("串阻", "振铃抑制 · 阻抗匹配", None, "// 22-33Ω 串阻"), ("差分走线", "USB/CAN 等长配对"), ("电源完整性", "纹波 · 去耦网络")]),
        ("联调排查", [("上电三查", "电压/电流/时钟"), ("晶振不起振", "负载电容/虚焊排查"), ("程序不跑", "BOOT 引脚/复位/供电")]),
        ("焊接与测试", [("手工焊接", "风枪/烙铁技巧"), ("飞线调试", "逻辑分析仪探针"), ("硬件 Debug", "示波器逐级排查")]),
    ]),
})
# __DEEP__
SUBS.update({
    "wireless": ("无线通信", [
        ("蓝牙 BLE", [("广播/连接", "广播包 → 扫描 → 连接建立", None, "// GAP 角色: 外设/中央"), ("GATT 服务", "服务/特征/属性 · 读写通知", None, "// CCCD 使能通知"), ("透传模块", "AT 指令控制 · 低门槛"), ("BLE 功耗", "广播间隔/连接间隔权衡")]),
        ("WiFi", [("ESP8266/ESP32", "AT 指令或 SDK 开发", None, "AT+CWJAP=\"ssid\",\"pass\""), ("配网方式", "SmartConfig/AirKiss/AP 配网"), ("连接云", "MQTT/TCP 上云", None, "AT+MQTTCONN")]),
        ("远距离", [("LoRa", "SX1276 扩频 · 数公里", None, "// 网关+节点拓扑"), ("NB-IoT", "运营商蜂窝 · 低功耗"), ("4G 模组", "AT 指令 · 高速数传")]),
        ("短距与子网", [("2.4G", "NRF24L01 · 点对点"), ("ZigBee/Thread", "802.15.4 网状网络"), ("433M 射频", "FSK · 简单透传")]),
        ("工程要点", [("天线", "PCB 天线/外置 · 阻抗匹配"), ("距离测试", "开阔地实测 · 环境干扰"), ("共存", "2.4G 多协议干扰")]),
        ("源码入口", [("ESP-AT 仓库", "WiFi/BLE 模组固件", "https://github.com/espressif/esp-at"), ("nRF5 SDK", "BLE 官方 SDK", "https://github.com/NordicSemiconductor/nRF5_SDK")]),
    ]),
    "storage_fs": ("存储与文件系统", [
        ("SD 卡", [("接口模式", "SDIO 高速 / SPI 兼容", None, "HAL_SD_Init(&hsd);"), ("初始化序列", "CMD0→ACMD41→读 CSD"), ("扇区读写", "512B 对齐 · 擦写寿命")]),
        ("FATFS", [("挂载与文件 API", "f_mount/f_open/f_write", None, "f_open(&fil, \"log.txt\", FA_WRITE|FA_OPEN_ALWAYS);"), ("长文件名", "LFN 缓冲配置"), ("掉电保护", "FAT 表损坏 · 备份策略")]),
        ("LittleFS", [("掉电安全", "COW 设计 · 无掉电损坏", None, "lfs_mount(&lfs, &cfg);"), ("磨损均衡", "均衡擦写 · 延长寿命"), ("适用", "NOR Flash 小容量")]),
        ("应用模式", [("数据记录仪", "周期写日志 · 满则轮转", None, "// 文件轮转: 按日期/大小"), ("固件存放", "SD 卡升级镜像"), ("配置存储", "INI/JSON 解析")]),
        ("源码入口", [("FatFs 官方", "文件系统库", "http://elm-chan.org/fsw/ff/00index_e.html"), ("LittleFS", "掉电安全文件系统", "https://github.com/littlefs-project/littlefs")]),
    ]),
    "gui": ("图形界面 GUI", [
        ("LVGL", [("对象树", "屏幕/对象/子对象层级", None, "lv_obj_t *scr = lv_scr_act();"), ("控件库", "按钮/标签/列表/图表", None, "lv_btn_create(scr);"), ("事件系统", "点击/长按/值变化", None, "lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);"), ("动画", "内置补间动画")]),
        ("显示适配", [("flush 回调", "像素搬运到屏幕", None, "lv_disp_drv_t disp_drv; disp_drv.flush_cb = my_flush;"), ("帧缓冲", "单缓冲/双缓冲/局部", None, "// 局部刷新节省内存"), ("DMA2D", "硬件加速图形搬运")]),
        ("输入设备", [("触摸屏", "电容/电阻 · 坐标校准"), ("编码器", "焦点导航"), ("按键矩阵", "事件映射")]),
        ("资源管理", [("字库", "内置/外部字体 · 取模", None, "// LV_FONT_MONTSERRAT_14"), ("图片", "转换 C 数组 · 压缩"), ("主题", "配色/样式定制")]),
        ("源码入口", [("LVGL 官方", "开源嵌入式 GUI", "https://github.com/lvgl/lvgl"), ("TouchGFX", "ST 官方 GUI", "https://github.com/STMicroelectronics/touchgfx")]),
    ]),
    "audio": ("音频接口", [
        ("I2S 协议", [("帧格式", "左右声道时分复用 · 位宽", None, "HAL_I2S_Init(&hi2s);"), ("主从模式", "BCLK/LRCLK 由主产生"), ("采样率", "8k-192k · 时钟分频")]),
        ("编解码器", [("WM8978/ES8388", "ADC+DAC 一体 · I2C 配置", None, "wm8978_init(96000);"), ("寄存器配置", "音量/增益/滤波"), ("耳机/麦克风", "模拟前端")]),
        ("播放与录音", [("DMA 双缓冲", "无缝音频流", None, "HAL_I2S_Transmit_DMA(&hi2s, buf, N);"), ("采样数据", "16 位 PCM · 格式转换"), ("静音/爆音处理", "软启动/渐变")]),
        ("应用", [("语音提示", "WAV 播放 · 词条拼接"), ("音频播放器", "文件系统读取解码"), ("语音识别前端", "关键字唤醒")]),
        ("源码入口", [("STM32CubeF4 音频例程", "官方 demo", "https://github.com/STMicroelectronics/STM32CubeF4/tree/master/Projects/STM32F4-Discovery/Applications/Audio")]),
    ]),
    "testing": ("软件测试与验证", [
        ("单元测试", [("Unity/CMock", "断言框架+桩生成", None, "TEST_ASSERT_EQUAL(5, add(2,3));"), ("宿主测试", "PC 上编译运行被测代码"), ("覆盖率", "语句/分支/MC/DC")]),
        ("集成测试", [("HIL 硬件在环", "真实硬件+自动化脚本", None, "// pytest + pyserial 驱动"), ("串口自动化", "命令-响应测试协议"), ("回归测试", "每次改动全量跑")]),
        ("静态分析", [("cppcheck", "空指针/越界/资源泄漏", None, "cppcheck --enable=all --std=c99 src/"), ("代码规范", "MISRA C 检查器"), ("编译警告", "-Wall -Wextra -Werror")]),
        ("生产测试", [("测试治具", "ICT/FCT 两阶段"), ("边界测试", "电压/温度/晶振频偏"), ("数据追溯", "序列号+测试报告")]),
        ("源码入口", [("Unity 测试框架", "轻量 C 测试", "https://github.com/ThrowTheSwitch/Unity"), ("Ceedling", "构建+测试一体化", "https://github.com/ThrowTheSwitch/Ceedling")]),
    ]),
    "industry": ("行业应用方向", [
        ("汽车电子", [("AUTOSAR", "标准化软件架构", None, "// BSW/RTE/ASW 分层"), ("ISO 26262", "功能安全 · ASIL 等级"), ("车载总线", "CAN/LIN/FlexRay 协同")]),
        ("工业控制", [("PLC 原理", "扫描周期 · 梯形图"), ("实时以太网", "EtherCAT/Profinet"), ("现场总线", "Modbus/CANopen/Profibus")]),
        ("IoT 物联网", [("云平台", "阿里云/腾讯云/OneNET", None, "// MQTT 三要素: 主题/发布/订阅"), ("设备管理", "影子设备 · OTA 升级"), ("边缘计算", "本地处理+云协同")]),
        ("消费电子", [("可穿戴", "低功耗+传感融合"), ("智能家居", "协议互通 · Matter"), ("家电", "电机控制+联网")]),
        ("方向选择", [("兴趣匹配", "汽车/工业/IoT 各需技能树"), ("技能复用", "RTOS/总线/可靠性格外重要"), ("成长路径", "从 MCU 到 SoC/Linux")]),
        ("源码入口", [("AUTOSAR 官网", "汽车软件标准", "https://www.autosar.org/"), ("Eclipse IoT", "开源物联网生态", "https://iot.eclipse.org/")]),
    ]),
})
DEEP = {}
# __DEEP__
DEEP.update({
    "deep_hardfault": ("HardFault 定位实战", [
        ("fault 寄存器", [("CFSR 细分", "MMFAR/BFAR 定位地址", None, "*(uint32_t*)0xE000ED28 /* CFSR */"), ("HFSR", "是否可恢复 · FORCED 位"), ("栈帧", "R0-R3/R12/LR/PC/xPSR 八件套")]),
        ("常见原因", [("野指针/数组越界", "写坏相邻对象"), ("栈溢出", "局部大数组/深递归 · MPU 可提前捕获"), ("非法指令/除零", "UsageFault 升级为 HardFault"), ("双指针问题", "回调被破坏")]),
        ("定位流程", [("1 读 CFSR", "确认错误类型"), ("2 看 LR", "EXC_RETURN 判断 MSP/PSP", None, "0xFFFFFFFD → 线程模式+PSP"), ("3 取栈帧 PC", "崩溃点指令地址 → 符号化"), ("4 回溯调用链", "LR 链式追溯")]),
        ("工具辅助", [("Keil 寄存器窗口", "fault 寄存器直接读"), ("GDB 脚本", "自动打印栈帧"), ("断言预防", "参数校验 · 越界检查先于访问")]),
        ("源码入口", [("core_cm4.h SCB", "fault 寄存器定义", "https://github.com/ARM-software/CMSIS_5")]),
    ]),
    "deep_rtos_sched": ("FreeRTOS 调度器专题", [
        ("核心机制", [("tick 节拍", "SysTick 中断 · 时间片推进", None, "configTICK_RATE_HZ 1000"), ("就绪链表", "每个优先级一条 · O(1) 取最高"), ("抢占点", "tick 中断/API 调用/中断退出")]),
        ("任务切换", [("PendSV 切换", "低优先级异常 · 完整现场保存", None, "vPortSVCHandler / xPortPendSVHandler"), ("现场保存", "R4-R11 手动 · 其余硬件自动"), ("SVC 启动", "第一个任务如何开始")]),
        ("常见坑", [("优先级反转", "互斥量继承机制解决"), ("优先级倒挂死锁", "任务等待顺序"), ("vTaskDelay vs 忙等", "让出 CPU · 别空转")]),
        ("源码入口", [("tasks.c", "调度核心", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/tasks.c"), ("port.c (ARM_CM4F)", "切换汇编", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM4F/port.c")]),
    ]),
    "deep_isr_rtos": ("中断与 RTOS 集成", [
        ("ISR 安全 API", [("FromISR 后缀", "xQueueSendFromISR 等 · 专用版本"), ("xHigherPriorityTaskWoken", "是否唤醒高优任务", None, "portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);"), ("危险 API", "vTaskDelay/xSemaphoreTake 禁用于 ISR")]),
        ("设计模式", [("延迟中断处理", "ISR 只发信号 · 任务处理业务"), ("二值信号量通知", "中断→任务经典模式", None, "BaseType_t xHigherPriorityTaskWoken = pdFALSE;"), ("队列直接送数据", "有界 · 背压")]),
        ("临界区", [("taskENTER_CRITICAL", "关中断+嵌套计数", None, "taskENTER_CRITICAL(); ... taskEXIT_CRITICAL();"), ("临界区时长", "越短越好 · 影响实时性")]),
        ("源码入口", [("queue.c", "FromISR 实现", "https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/queue.c")]),
    ]),
    "deep_uart_dma": ("UART 不定长接收实战", [
        ("方案对比", [("轮询", "阻塞 · 简单场景"), ("单字节中断", "环形缓冲 · 每字节一次中断"), ("空闲中断+DMA", "黄金方案 · 零中断开销", None, "HAL_UARTEx_ReceiveToIdle_DMA(&huart, buf, MAX);")]),
        ("实现要点", [("半字/满字回调", "HAL_UARTEx_RxEventCallback"), ("帧判定", "空闲超时=一帧结束"), ("环形缓冲+DMA", "双缓冲无缝 · 防覆盖")]),
        ("常见问题", [("丢帧", "缓冲满未消费 · 增大或流控"), ("粘包", "协议加长度/分隔符"), ("DMA 与 CPU 竞争", "完成后再处理 · 别边收边读")]),
        ("源码入口", [("stm32f4xx_hal_uart.c", "RxEvent 实现", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c")]),
    ]),
    "deep_clock_tree": ("时钟树配置专题", [
        ("配置流程", [("1 开 HSE", "等待就绪 · CSS 检测"), ("2 配 PLL", "M/N/P 分频 · VCO 范围", None, "RCC_PLL_M=8, N=336, P=2 → 168MHz"), ("3 切 SYSCLK", "FLASH 等待周期先配好")]),
        ("分频要点", [("AHB/APB1/APB2", "168/4=42M / 168/2=84M 上限"), ("定时器时钟", "APB1 分频≠1 时定时器 ×2"), ("外设时钟树", "UART/TIM/ADC 各有源")]),
        ("常见错误", [("FLASH 等待未配", "高速运行随机故障", None, "__HAL_FLASH_SET_LATENCY(FLASH_LATENCY_5);"), ("PLL 超 VCO 范围", "锁不住 · 系统诡异"), ("外设时钟忘开", "读寄存器全 0")]),
        ("源码入口", [("system_stm32f4xx.c", "SystemInit 时钟", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Projects/STM32F4-Discovery/Templates/Src/system_stm32f4xx.c")]),
    ]),
    "deep_boot_mcu": ("启动流程详解", [
        ("上电瞬间", [("向量表", "0x08000000 栈顶 + 复位向量", None, "__initial_sp / Reset_Handler"), ("BOOT 引脚", "决定首条指令来源")]),
        ("startup 汇编", [("栈与堆", "Stack_Size/Heap_Size 定义"), ("向量表定义", "全部异常/中断入口", None, "DCD Reset_Handler; DCD NMI_Handler;"), ("启动顺序", "清 BSS → 拷 DATA → SystemInit → main")]),
        ("C 环境就绪", [("SystemInit", "Flash 等待 + 时钟"), ("__libc_init_array", "C++ 全局构造(如用)"), ("main 之后", "外设初始化 → 主循环/RTOS")]),
        ("常见启动失败", [("向量表错位", "跳转后 HardFault"), ("栈不够", "启动即崩 · 改 Stack_Size"), ("时钟未稳就切", "系统跑飞")]),
        ("源码入口", [("startup_stm32f4xx.s", "启动汇编模板", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Projects/STM32F4-Discovery/Templates/Src/startup_stm32f4xx.s")]),
    ]),
    "deep_motor": ("BLDC 电机控制专题", [
        ("六步换相", [("换相顺序", "6 个通电状态 · 每 60° 电角度切换", None, "// 换相表: 6 组 PWM 状态"), ("反电动势检测", "悬空相过零检测"), ("启动策略", "对齐转子 → 强制换相 → 闭环")]),
        ("PWM 策略", [("上桥 PWM/下桥常通", "经典调制"), ("同步整流", "提高效率"), ("死区时间", "防桥臂直通")]),
        ("闭环控制", [("电流环", "采样电阻 + ADC 同步"), ("速度环", "换相周期估算转速"), ("位置环", "编码器/霍尔")]),
        ("保护", [("过流保护", "比较器硬件快速关断", None, "// TIM 刹车输入"), ("过温保护", "NTC 监测"), ("堵转检测", "转速异常判定")]),
        ("源码入口", [("STM32 MCSDK", "电机控制库", "https://github.com/STMicroelectronics/motor-control-sdk")]),
    ]),
    "deep_i2c": ("I2C 时序与故障专题", [
        ("时序细节", [("起始/停止", "SCL 高电平沿变化", None, "// SDA 在 SCL 高时跳变"), ("字节与 ACK", "8 位数据 + 1 位应答"), ("重复起始", "读后写 · 寄存器寻址")]),
        ("死锁恢复", [("SDA 卡低", "从机异常 · 9 时钟脉冲", None, "// GPIO 模拟 SCL 脉冲"), ("时钟拉伸", "从机慢速 · 主等"), ("上拉失效", "阻值过大/虚焊")]),
        ("速率与负载", [("上拉计算", "400k 需 1-2kΩ · 总线电容"), ("多从机", "地址冲突 · 总线长度")]),
        ("调试", [("逻辑分析仪", "抓时序对图"), ("回读验证", "写后读 · 寄存器确认")]),
    ]),
    "deep_usb": ("USB 枚举与 CDC 专题", [
        ("枚举流程", [("复位", "主机检测设备接入"), ("地址分配", "SET_ADDRESS"), ("读描述符", "设备→配置→接口→端点", None, "USBD_GetDeviceDescriptor(&dev->pData, ...);"), ("配置完成", "SET_CONFIGURATION")]),
        ("CDC 虚拟串口", [("类结构", "通信接口+数据接口"), ("端点规划", "通知+批量收发"), ("数据流", "环形缓冲 ↔ USB FIFO")]),
        ("常见问题", [("枚举失败", "D+ 上拉 · 描述符错误"), ("速率不稳", "时钟精度 · 晶体要求"), ("拔插异常", "状态机未复位")]),
        ("调试", [("总线分析仪", "抓包看枚举"), ("USBTrace", "软件抓包")]),
    ]),
    "deep_lowpower": ("低功耗实战专题", [
        ("功耗模型", [("三部分", "运行+睡眠+唤醒转换"), ("指标", "平均电流 = Σ(状态电流×占比)"), ("测量", "串联电流表/μA 档")]),
        ("外设管理", [("时钟门控", "关无用外设时钟", None, "__HAL_RCC_USART1_CLK_DISABLE();"), ("GPIO 状态", "浮空输入耗电 · 固定电平"), ("DMA/定时器", "按需使能")]),
        ("唤醒流程", [("Stop 唤醒", "时钟重新就绪 · 恢复外设", None, "HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);"), ("Standby 唤醒", "复位式 · 数据需备份"), ("tickless 配合", "RTOS 自动管理")]),
        ("验证", [("功耗测试", "各模式实测电流表"), ("唤醒时间", "示波器测量"), ("长期稳定性", "电池续航估算")]),
    ]),
    "deep_pid": ("PID 控制算法专题", [
        ("控制基础", [("闭环 vs 开环", "反馈修正"), ("P 比例", "当前误差 · 快速响应", None, "out = Kp * error;"), ("I 积分", "累计误差 · 消除静差"), ("D 微分", "误差变化率 · 抑制超调")]),
        ("实现要点", [("位置式 vs 增量式", "增量式更安全", None, "out += Kp*(e-e1) + Ki*e + Kd*(e-2*e1+e2);"), ("积分限幅", "防积分饱和"), ("输出限幅", "执行器范围")]),
        ("调参方法", [("经验法", "先 P 后 I 再 D"), ("Ziegler-Nichols", "临界振荡法"), ("仿真验证", "MATLAB/在线仿真")]),
        ("应用", [("电机调速", "速度环"), ("平衡车", "角度环+速度环"), ("温控", "大惯性 · 慢环")]),
    ]),
    "deep_ringbuf": ("环形缓冲与内存池设计", [
        ("环形缓冲", [("结构", "head/tail 索引 · 容量 2^n", None, "// size 取 2 的幂 用 & 运算"), ("读写规则", "生产者写 tail · 消费者读 head"), ("满/空判断", "计数法 vs 留空一格")]),
        ("无锁单对单", [("原子索引", "单生产者单消费者免锁"), ("内存屏障", "写入顺序保证")]),
        ("内存池", [("固定块", "预分配数组 · 空闲链表", None, "// block[64] 空闲链"), ("分配/释放", "O(1) · 无碎片"), ("统计", "使用峰值/泄漏检测")]),
        ("实战", [("串口接收", "ISR 写入 · 任务读取"), ("事件队列", "任务间消息"), ("DMA 双缓冲", "无缝采集")]),
    ]),
    "deep_canfd": ("CAN FD 与车载协议专题", [
        ("CAN FD 特性", [("可变速率", "仲裁段 500k · 数据段 5M", None, "// BRS 位切换"), ("更长载荷", "64 字节 · CRC 增强"), ("兼容", "FD 帧与经典帧混跑")]),
        ("错误分析", [("错误帧风暴", "单节点故障拖垮总线"), ("位时序错误", "采样点漂移"), ("总线关闭恢复", "静默重连策略")]),
        ("UDS 诊断", [("诊断会话", "默认/编程/扩展", None, "// 0x10 会话控制"), ("常用服务", "0x22 读数据 · 0x2E 写数据 · 0x31 例程"), ("DTC", "故障码管理")]),
        ("网络管理", [("OSEK NM", "节点睡眠/唤醒协同"), ("心跳机制", "节点在线监测")]),
    ]),
    "deep_secureboot": ("安全启动与固件保护", [
        ("信任链", [("信任根", "ROM 代码/OTP 公钥", None, "// 首级验证"), ("逐级验证", "BOOT 验 APP 签名"), ("回滚防护", "版本号单调递增")]),
        ("签名方案", [("非对称签名", "ECDSA/RSA · 私钥签", None, "// 公钥验签"), ("哈希", "SHA-256 摘要"), ("密钥管理", "私钥离线保管 · 防泄露")]),
        ("固件加密", [("下载加密", "传输层 AES"), ("存储加密", "Flash 密文存储")]),
        ("防抄板", [("UID 绑定", "密钥派生", None, "// 一机一密"), ("读保护", "RDP 等级选择"), ("调试口", "量产关闭 JTAG/SWD")]),
    ]),
})
ROADMAP = [
    ("阶段 0 · 准备", "工具与环境就绪", ["数电基础（电平/上拉/时序图）", "C 指针/结构体/位操作", "Keil/GCC 建工程 · 烧录一条龙", "逻辑分析仪/示波器上手"], "板子点灯成功", "electronics"),
    ("阶段 1 · 点亮世界", "GPIO 与串口", ["GPIO 模式/上下拉/开漏推挽", "按键轮询+EXTI 中断", "UART 轮询收发 + printf 重定向"], "按键控制 LED + 串口打印", "gpio"),
    ("阶段 2 · 时间与模拟", "定时器与 ADC", ["SysTick 延时 · 通用定时器", "PWM（调光/舵机）", "输入捕获（测频率）", "ADC 单通道+DMA 采集"], "PWM 呼吸灯 + ADC 电压显示", "timer"),
    ("阶段 3 · 通信协议", "UART/SPI/I2C/CAN", ["UART 环形缓冲 · 空闲+DMA", "SPI 驱动 OLED/Flash", "I2C 读写 EEPROM", "CAN 收发+滤波器"], "双机 CAN 通信 + OLED 显示", "uart"),
    ("阶段 4 · 系统化", "FreeRTOS 多任务", ["任务创建/优先级/时间片", "队列/信号量/互斥量", "中断与 RTOS 集成", "内存管理 heap 选择"], "RTOS 下三任务协作项目", "rtos"),
    ("阶段 5 · 工程化", "低功耗与可靠性", ["Sleep/Stop/Standby", "看门狗策略 · 掉电保存", "IAP 双区升级 · 回滚", "HardFault 快速定位"], "带 OTA 升级的产品原型", "lowpower"),
    ("阶段 6 · 实战输出", "完整项目与源码", ["读 HAL/FreeRTOS 源码", "状态机架构 · 分层设计", "量产：读保护/UID/测试"], "完成一个可交付项目", "software_arch"),
]
# __ROADMAP__
RES = {
    "官方资料": [("STM32CubeF4 (GitHub)", "HAL 源码+例程 · 第一手", "https://github.com/STMicroelectronics/STM32CubeF4"), ("STM32 手册合集", "参考手册/数据手册/编程手册(含中文)", "https://github.com/NorthQian/STM32_Manual"), ("FreeRTOS 官方文档(中文)", "任务/队列/信号量权威说明", "https://www.freertos.org/Documentation/"), ("CMSIS_5", "Cortex-M 内核寄存器定义", "https://github.com/ARM-software/CMSIS_5"), ("OpenOCD", "开源调试烧录", "https://openocd.org/")],
    "书籍推荐(按序)": [("《Cortex-M3 权威指南》(中文)", "内核机制圣经 · 必读第一本"), ("《FreeRTOS 源码与应用开发实战》", "源码级 RTOS 理解"), ("《STM32 嵌入式系统开发实战指南》", "工程实践套路"), ("《嵌入式系统软件设计中的常用算法》", "滤波/控制/协议算法"), ("《程序员的自我修养》", "链接装载与库(进阶)")],
    "视频教程": [("江协科技 STM32 入门教程(B站)", "华语区最系统的入门课", "https://www.bilibili.com/video/BV1th411z7sn"), ("嵌入式 Linux/单片机频道", "进阶主题持续更新")],
    "常见误区纠正": [("误区: 必须用寄存器不用 HAL", "正确: HAL 可读性好 · 关键处再寄存器"), ("误区: 中断里 printf/延时", "正确: 中断快进快出 · 置标志"), ("误区: 全局变量随便用", "正确: volatile+临界区 · 接口化"), ("误区: 看门狗喂了就完", "正确: 任务级喂狗 · 防假死"), ("误区: 烧录失败就换线", "正确: 查供电/时钟/读保护/接线"), ("误区: 程序不跑就重刷", "正确: 先查时钟/复位/BOOT 引脚")],
    "面试高频(MCU 向)": [("GPIO 推挽 vs 开漏"), ("EXTI 与 NVIC 的关系"), ("中断优先级分组怎么配"), ("DMA 为什么能省 CPU"), ("UART 接收如何不丢数据"), ("FreeRTOS 任务间怎么通信"), ("优先级反转与互斥量"), ("低功耗唤醒后程序从哪继续"), ("IAP 跳转要注意什么"), ("HardFault 怎么定位"), ("SPI 四种模式怎么选"), ("CAN 仲裁原理"), ("环形缓冲怎么实现"), ("栈溢出怎么检测")],
}
# __RES__
NOTES = {
    "n_gpio_mode": {"text": "GPIO 四种模式：输入/输出/复用/模拟。输出又分推挽（强驱动、推拉电平）与开漏（只能拉低、靠外部上拉拉高，用于线与与电平转换）。按键输入必须配上拉/下拉否则悬空乱跳。", "links": [("HAL GPIO 源码", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c")]},
    "n_exti": {"text": "EXTI 外部中断：16 根中断线按端口共享（PA0-PG0 都走 EXTI0），同一编号的引脚不能同时用。HAL 流程：配置引脚为中断模式 → HAL_NVIC_EnableIRQ → 在 HAL_GPIO_EXTI_Callback 里写处理。回调里别放耗时逻辑。", "links": [("江协科技 STM32 入门教程(B站)", "https://www.bilibili.com/video/BV1th411z7sn")]},
    "n_prio": {"text": "NVIC 优先级分组把 4 位优先级拆成抢占+子优先级（如分组 4 = 4 位抢占 0 位子）。抢占级可嵌套打断，子优先级只在同抢占级内排队。RTOS 场景通常用分组 4，并全局规划各中断优先级。", "links": []},
    "n_critical": {"text": "临界区三件套：PRIMASK 关全部中断（__disable_irq）、BASEPRI 关低于某级的中断（更精细）、FreeRTOS 的 taskENTER_CRITICAL（嵌套计数+关中断）。原则：临界区越短越好，禁止在里面调可能阻塞的代码。", "links": []},
    "n_pwm": {"text": "PWM 频率 = 定时器时钟/(PSC+1)/(ARR+1)，占空比 = CCR/(ARR+1)。调光/舵机用通用定时器即可，电机驱动用高级定时器（带死区与互补输出）。改占空比用 __HAL_TIM_SET_COMPARE，别在中断里反复启停定时器。", "links": []},
    "n_uart_dma": {"text": "UART 接收三大方案：轮询（阻塞，简单）、单字节中断+环形缓冲（每字节一次中断）、空闲中断+DMA（零中断开销，不定长帧的黄金方案）。关键在 HAL_UARTEx_RxEventCallback 里判定帧边界，用双缓冲防覆盖。", "links": [("HAL UART 源码", "https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c")]},
    "n_spi": {"text": "SPI 全双工 4 线：SCK/MOSI/MISO/CS。CPOL 决定空闲电平，CPHA 决定采样沿，共 4 种模式，必须与从机匹配。读操作也是先发后收（全双工），注意 CS 拉低时序。", "links": []},
    "n_i2c": {"text": "I2C 两线开漏：SCL+SDA，靠上拉电阻拉高，线与仲裁天然支持多主。7 位地址 + 读/写位。HAL_I2C_Mem_Read/Write 是访问带寄存器地址器件（EEPROM/传感器）的标准接口。总线卡死可用 9 个时钟脉冲恢复。", "links": []},
    "n_can": {"text": "CAN 总线差分传输，抗干扰强，汽车/工业标配。ID 即优先级（越小越优先），仲裁无损。波特率由 BRP+时序段决定，采样点建议 75%-85%。滤波器组用掩码或列表模式只收关心的帧。", "links": []},
    "n_dma": {"text": "DMA 让外设数据不经过 CPU 直达内存，UART/ADC/SPI 高吞吐场景必用。三种方向+单次/循环模式。循环模式+双缓冲可做到无缝采集。注意缓冲对齐与 DMA 完成后再处理数据，避免撕裂。", "links": []},
    "n_adc": {"text": "STM32 ADC 是 12 位逐次逼近型：采样时间决定输入阻抗匹配，通道序列可自动扫描，DMA 模式把结果连续写入数组。电压换算 = adc×Vref/4095。多通道要注意通道间串扰与采样顺序。", "links": []},
    "n_freertos_task": {"text": "任务 = 独立栈 + 优先级 + 状态机（就绪/运行/阻塞/挂起）。xTaskCreate 分配栈空间，栈给多大是门学问（用 uxTaskGetStackHighWaterMark 看水位）。vTaskDelay 让出 CPU，别用忙等延时。", "links": [("FreeRTOS 官方文档(中文)", "https://zhop.freertos.org/zh-cn-cmn-s/Documentation/")]},
    "n_queue": {"text": "队列是任务间通信的主力：发送方拷贝入队，接收方阻塞等待。xQueueSend/Receive 的阻塞时间参数是精髓（0=不等待，portMAX_DELAY=无限等）。中断里用 FromISR 版本并配合任务唤醒。", "links": []},
    "n_mutex": {"text": "互斥量 = 带优先级继承的二值信号量：低优先级任务持锁时，高优先级任务等待会把持锁者临时提权，防止优先级反转（高优任务被中优任务饿死）。这是信号量与互斥量的本质区别。", "links": []},
    "n_heap": {"text": "FreeRTOS 内存管理 5 个 heap 实现：heap_1 只分配不释放（简单），heap_2 无合并，heap_3 包标准 malloc（线程安全），heap_4 合并相邻块（最常用），heap_5 支持多段内存。选型看碎片与释放需求。", "links": []},
    "n_wdt": {"text": "IWDG 独立看门狗用内部 RC，主程序跑飞也能复位；WWDG 窗口看门狗要求在规定窗口内喂狗，能抓'提前喂'的异常。高级做法：任务级喂狗（每个任务报活，超时统一复位）防假死。", "links": []},
    "n_iap": {"text": "IAP 双区升级：BOOT 区校验 APP 区（CRC+版本）→ 有效则跳转，无效则等待下载。跳转三要素：关中断、重设栈顶、取复位向量。升级过程断电要有回滚保护。", "links": []},
    "n_hardfault": {"text": "HardFault 定位四步：① 读 CFSR 确认错误类型（越界/总线/用法）② 看 LR 的 EXC_RETURN 判断用 MSP 还是 PSP ③ 从对应栈指针取 8 个栈帧，PC 就是崩溃点 ④ 符号化（map 文件/addr2line）并沿 LR 回溯调用链。", "links": [("Cortex-M3 权威指南(中文)", "https://etcnew.sdut.edu.cn/meol/homepage/common/attribute_file.jsp?_style=new06&lid=45050&resid=626545")]},
    "n_systick": {"text": "SysTick 24 位倒计时器，FreeRTOS 的 tick 来源。HAL_Delay 依赖它（且必须在中断使能状态）。RTOS 下别用 HAL_Delay（阻塞调度），用 vTaskDelay。", "links": []},
    "n_clock_pll": {"text": "PLL 配置核心：VCO 频率 = HSE/M×N 必须落在规定范围（如 F4 的 100-432MHz），输出 = VCO/P。先配 FLASH 等待周期再切时钟，否则高速运行随机故障。APB1 分频≠1 时其定时器时钟翻倍，这是经典坑。", "links": []},
    "n_motor": {"text": "电机三大类：有刷直流（H 桥+占空比调速）、无刷 BLDC（六步换相+反电动势检测）、步进（脉冲+方向+细分）。控制核心是 PWM 频率选择（20kHz 超声区）与 PID 闭环。", "links": []},
    "n_usb": {"text": "USB 枚举是主机发现设备的握手：复位→地址→描述符→配置。CDC 虚拟串口是最常用的免驱类。枚举失败先查 D+ 上拉电阻与 VBUS 检测，描述符错误看抓包。", "links": []},
    "n_eth": {"text": "以太网 = 内嵌 MAC + 外置 PHY（RMII 接口 50M 时钟）。LwIP 提供 TCP/IP 协议栈，PBUF 内存管理是理解重点。链路问题先读 PHY 寄存器看协商状态。", "links": []},
    "n_modbus": {"text": "Modbus RTU 是工业最普及的协议：主从一问一答，地址+功能码+数据+CRC16。RS485 半双工注意方向切换时序（发完等最后一个字节送完再拉低 DE）。", "links": []},
    "n_state_machine": {"text": "状态机 = 事件驱动的健壮架构：状态+事件+转移表。协议解析、按键防抖、界面导航都是经典应用。层次状态机解决嵌套状态复用，转移表比 switch-case 更可维护。", "links": []},
    "n_misra": {"text": "MISRA C 是汽车/工业嵌入式编码规范：禁止动态内存、严格类型、限制 goto 等。工具（cppcheck/PC-lint）可自动检查。安全认证项目（IEC 61508）强制要求。", "links": [("MISRA 官网", "https://www.misra.org.uk/")]},
    "n_bldc": {"text": "BLDC 控制主线：六步换相（6 个通电状态）→ 反电动势过零检测换相 → 电流环+速度环闭环。PWM 用互补输出+死区，过流用定时器刹车输入硬件保护。", "links": []},
    "n_pid": {"text": "PID 三件：P 快响应、I 消静差、D 抑超调。嵌入式实现要点：增量式更安全、积分限幅防饱和、输出限幅、抗积分饱和（积分分离）。调参：先 P 到临界振荡，再 I 消差，最后 D 压超调。", "links": []},
    "n_canfd": {"text": "CAN FD 在仲裁段后切换到 5Mbps 高速数据段（BRS 位），载荷 64 字节，CRC 增强。与经典 CAN 可混跑。汽车诊断用 UDS（0x22 读/0x2E 写/0x31 例程），网络管理用 OSEK NM。", "links": []},
    "n_secureboot": {"text": "安全启动 = 信任链：ROM 信任根 → BOOT 验 APP 签名（ECDSA/RSA + SHA-256）→ 回滚防护（版本单调）。固件下载加密防抓包，UID 绑定实现一机一密防抄板，量产关闭调试口。", "links": []},
}
# __NOTES__
CODE_EXTRA = {
    "mcu_arch|异常向量表": "SCB->VTOR = FLASH_BASE;",
    "mcu_arch|MPU": "MPU->RBAR = addr; MPU->RASR = size | AP;",
    "boot|向量表加载": "ldr r0, =SystemInit; blx r0; ldr r0, =main; bx r0;",
    "boot|volatile": "volatile uint32_t flag; /* 中断共享变量 */",
    "boot|分散加载": "LR_IROM1 0x08000000 0x100000 { ... }",
    "clock|HSI/HSE": "RCC_HSE_ON → 等待就绪",
    "clock|外设时钟使能": "__HAL_RCC_GPIOA_CLK_ENABLE();",
    "clock|SysTick": "SysTick_Config(SystemCoreClock / 1000);",
    "clock|看门狗 IWDG": "HAL_IWDG_Refresh(&hiwdg);",
    "gpio|推挽 vs 开漏": "GPIO_MODE_OUTPUT_PP / GPIO_MODE_OUTPUT_OD",
    "gpio|写引脚": "HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, SET);",
    "gpio|EXTI 中断": "HAL_GPIO_EXTI_Callback(GPIO_PIN_0);",
    "nvic|优先级分组": "HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);",
    "nvic|ISR 编写规范": "void EXTI0_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0); }",
    "nvic|PRIMASK": "__disable_irq(); ... __enable_irq();",
    "nvic|现场取证": "MRS R0, PSP; /* 读线程栈指针 */",
    "timer|时基单元": "__HAL_TIM_SET_AUTORELOAD(&htim, 999);",
    "timer|PWM 输出": "__HAL_TIM_SET_COMPARE(&htim, TIM_CHANNEL_1, 500);",
    "timer|输入捕获": "HAL_TIM_IC_CaptureCallback(&htim);",
    "uart|帧格式": "HAL_UART_Transmit(&huart1, buf, len, 100);",
    "uart|环形缓冲": "ringbuf_put(&rb, byte);",
    "spi|4 线制": "HAL_SPI_Transmit(&hspi, buf, n, 100);",
    "i2c|两线制": "HAL_I2C_Mem_Read(&hi2c1, addr, reg, 1, buf, n, 100);",
    "can|仲裁机制": "CAN_ID_STD | CAN_RTR_DATA",
    "can|滤波器": "HAL_CAN_ConfigFilter(&hcan, &sFilterConfig);",
    "can|错误计数": "if (hcan.ErrorCode & HAL_CAN_ERROR_BOF) { 复位重启; }",
    "dma|传输模式": "HAL_UART_Receive_DMA(&huart, buf, N);",
    "dma|ADC 采集": "HAL_ADC_Start_DMA(&hadc, buf, N);",
    "adc|采样时间": "HAL_ADC_Start(&hadc); HAL_ADC_PollForConversion(&hadc, 10);",
    "adc|扫描多通道": "HAL_ADC_Start_DMA(&hadc, buf, 8);",
    "adc|电压换算": "v = (float)adc * 3.3f / 4095.0f;",
    "adc|DMA 播放": "HAL_DAC_Start_DMA(&hdac, ch, buf, n, DAC_ALIGN_12B_R);",
    "rtos|任务创建": "xTaskCreate(vTask1, \"t1\", 128, NULL, 2, NULL);",
    "rtos|时间片": "vTaskDelay(pdMS_TO_TICKS(100));",
    "rtos|队列": "xQueueSend(q, &val, 0); xQueueReceive(q, &val, portMAX_DELAY);",
    "rtos|中断集成": "portYIELD_FROM_ISR(xHigherPriorityTaskWoken);",
    "lowpower|看门狗策略": "HAL_IWDG_Refresh(&hiwdg);",
    "lowpower|IAP 设计": "((void (*)(void))APP_ADDR)();",
    "lowpower|读保护": "FLASH_OB_RDP_LevelConfig(OB_RDP_LEVEL_1);",
    "lowpower|CRC 校验": "HAL_CRC_Calculate(&hcrc, buf, len);",
    "toolchain|Keil uVision": "UV4 -b project.uvprojx -o build.log",
    "toolchain|GCC/CMake": "arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -O2",
    "toolchain|调试器": "openocd -f interface/stlink.cfg -f target/stm32f4x.cfg",
    "toolchain|代码生成": "// MX_GPIO_Init(); MX_USART1_UART_Init();",
    "debug_adv|ITM/SWO": "ITM_SendChar(c);",
    "debug_adv|DWT": "DWT->CYCCNT",
    "debug_adv|函数耗时": "GPIOE->ODR ^= 1<<1; // 测量区间",
    "reliability|去耦设计": "// 100nF 每 IC + 10uF 储能",
    "reliability|MISRA C": "// cppcheck --enable=all",
    "security|加密算法": "HAL_CRYP_AES_Encrypt(&hcryp, in, out, 1000);",
    "security|RNG 随机数": "HAL_RNG_GenerateRandomNumber(&hrng, &val);",
    "security|TLS 最小化": "mbedtls_ssl_handshake(&ssl);",
    "electronics|上拉/下拉": "R = (Vcc - Vih) / Ipu",
    "electronics|分压/限流": "R = (3.3 - 2.0) / 0.01",
    "sensors|IMU": "HAL_I2C_Mem_Read(&hi2c, 0x68<<1, 0x3B, 1, acc, 6, 100);",
    "sensors|OLED": "OLED_ShowString(0, 0, \"HELLO\");",
    "pcb|晶振布局": "// CL = (C1*C2)/(C1+C2) + Cs",
    "deep_hardfault|CFSR 细分": "*(uint32_t*)0xE000ED28 /* CFSR */",
    "deep_hardfault|看 LR": "0xFFFFFFFD → 线程模式+PSP",
    "deep_rtos_sched|tick 节拍": "configTICK_RATE_HZ 1000",
    "deep_rtos_sched|任务切换": "vPortSVCHandler / xPortPendSVHandler",
    "deep_isr_rtos|FromISR 后缀": "xQueueSendFromISR(q, &v, &xHigherPriorityTaskWoken);",
    "deep_isr_rtos|延迟中断处理": "BaseType_t xHigherPriorityTaskWoken = pdFALSE;",
    "deep_uart_dma|空闲中断+DMA": "HAL_UARTEx_ReceiveToIdle_DMA(&huart, buf, MAX);",
    "deep_clock_tree|配 PLL": "RCC_PLL_M=8, N=336, P=2 → 168MHz",
    "deep_clock_tree|FLASH 等待": "__HAL_FLASH_SET_LATENCY(FLASH_LATENCY_5);",
    "deep_boot_mcu|向量表": "__initial_sp / Reset_Handler",
    "deep_boot_mcu|启动顺序": "DCD Reset_Handler; DCD NMI_Handler;",
    "deep_motor|六步换相": "// 换相表: 6 组 PWM 状态",
    "deep_i2c|死锁恢复": "// GPIO 模拟 SCL 脉冲",
    "deep_usb|读描述符": "USBD_GetDeviceDescriptor(&dev->pData, ...);",
    "deep_lowpower|时钟门控": "__HAL_RCC_USART1_CLK_DISABLE();",
    "deep_lowpower|Stop 唤醒": "HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);",
    "deep_pid|P 比例": "out = Kp * error;",
    "deep_pid|位置式 vs 增量式": "out += Kp*(e-e1) + Ki*e + Kd*(e-2*e1+e2);",
    "deep_ringbuf|结构": "// size 取 2 的幂 用 & 运算",
    "deep_canfd|可变速率": "// BRS 位切换",
    "deep_canfd|UDS 诊断": "// 0x10 会话控制",
    "deep_secureboot|信任根": "// 首级验证",
    "deep_secureboot|UID 绑定": "// 一机一密",
}

SUB_NAMES = {k: v[0] for k, v in {**SUBS, **DEEP}.items()}
SUB_NAMES.update({"roadmap": "学习路线图", "resources": "资源与误区"})
# __CODE_EXTRA__

GROUP_COLORS = {
    "mcu_arch": ("#1a5276", "#5dade2"), "boot": ("#7d6608", "#f4d03f"), "clock": ("#b7950b", "#f1c40f"),
    "gpio": ("#1e8449", "#58d68d"), "nvic": ("#c0392b", "#f1948a"), "timer": ("#d35400", "#ffa64d"),
    "uart": ("#2471a3", "#7fb3e8"), "can": ("#6c3483", "#af7ac5"), "dma": ("#0e6655", "#48c9b0"),
    "adc": ("#148f77", "#76d7c4"), "rtos": ("#515a5a", "#aeb6bf"), "lowpower": ("#4d5656", "#99a3a4"),
    "toolchain": ("#7d6608", "#f4d03f"), "mem_bus": ("#1a5276", "#5dade2"), "pwm_motor": ("#d35400", "#ffa64d"),
    "usb": ("#2471a3", "#7fb3e8"), "eth": ("#1a5276", "#5dade2"), "spi": ("#2471a3", "#7fb3e8"),
    "i2c": ("#2471a3", "#7fb3e8"), "rtos_adv": ("#515a5a", "#aeb6bf"), "memory_mgmt": ("#4d5656", "#99a3a4"),
    "software_arch": ("#6c3483", "#af7ac5"), "debug_adv": ("#148f77", "#76d7c4"), "reliability": ("#c0392b", "#f1948a"),
    "security": ("#6c3483", "#af7ac5"), "electronics": ("#1e8449", "#58d68d"), "sensors": ("#1e8449", "#58d68d"),
    "pcb": ("#1e8449", "#58d68d"), "wireless": ("#2471a3", "#7fb3e8"), "storage_fs": ("#7d6608", "#f4d03f"),
    "gui": ("#6c3483", "#af7ac5"), "audio": ("#148f77", "#76d7c4"), "testing": ("#c0392b", "#f1948a"),
    "industry": ("#515a5a", "#aeb6bf"), "deep": ("#1f3a5f", "#5b8dd9"),
}

C = dict(bg="#15181c", card="#1f2429", card_border="#2c333d", line="#3a4350",
         text="#e8edf3", dim="#93a0b0", accent="#4f8cff", accent_t="#9fc3ff",
         layer="#20252b", layer_border="#39424e", item="#262c33", item_border="#3a4350",
         title_bar="#1c2e4a", title_border="#3b5b8a", col_head="#2a323d", col_border="#41506a",
         warn_fill="#3d3413", warn_border="#8a6d1f", warn_text="#f0d98c")

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def esc_attr(s):
    return esc(s).replace('"', "&quot;")

def build_svg(W, H_, parts):
    return ("\n".join([f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H_}" width="{W}" height="{H_}">',
                       f'<rect width="{W}" height="{H_}" fill="{C["bg"]}"/>'] + parts + ["</svg>"]))

def srect(x, y, w, h, fill, stroke, sw=2, rx=12, extra=""):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{extra}/>'

def stext(cx, cy, s, fs, fill=C["text"], anchor="middle", bold=False):
    w = ' font-weight="700"' if bold else ""
    return f'<text x="{cx}" y="{round(cy + fs * 0.35, 1)}" font-family="{FAM}" font-size="{fs}" fill="{fill}" text-anchor="{anchor}"{w}>{esc(s)}</text>'

def sltext(x, y, s, fs, fill=C["text"], bold=False):
    return stext(x, y, s, fs, fill, anchor="start", bold=bold)

def title_bar(W, title):
    return [srect(30, 26, W - 60, 64, C["title_bar"], C["title_border"], rx=12),
            stext(W / 2, 58, title + "  ·  子知识结构图", 25, "#FFFFFF", bold=True)]

def item_box(x, y, w, name, detail, target=None, src=None, code=None, h=44, note=None):
    extra = ' class="cbox"'
    if target:
        extra += f' data-target="{target}"'
    if src:
        extra += f' data-src="{src}"'
    if note:
        extra += f' data-note="{note}"'
    extra += f' data-name="{esc_attr(name)}" data-detail="{esc_attr(detail)}"'
    if code:
        extra += f' data-code="{esc_attr(code)}"'
        m = _re.search(r"([A-Za-z_][A-Za-z0-9_]*)", code)
        if m:
            extra += f' data-sym="{m.group(1)}"'
    out = [srect(x, y, w, h, C["item"], "#3d4754" if (target or src) else C["item_border"], 1.5, 8, extra=extra)]
    if target:
        out.append(f"<title>点击进入专题：{detail}</title>")
    elif src:
        out.append(f"<title>打开资料：{src[:60]}</title>")
    color = "#7ab8ff" if target else ("#8ce0ac" if src else C["text"])
    marker = ("🔗 " if target else "") + ("📄 " if src else "")
    if code:
        out.append(sltext(x + 14, y + 15, marker + name, 16, color, bold=True))
        out.append(f'<text x="{x + 14}" y="{round(y + 34, 1)}" font-family="Consolas, monospace" font-size="13" fill="#f0d98c" text-anchor="start">{esc(code)}</text>')
        out.append(sltext(x + 14, y + 52, detail, 12.5, C["dim"]))
    else:
        out.append(sltext(x + 14, y + 18, marker + name, 16.5, color, bold=True))
        out.append(sltext(x + 14, y + 36, detail, 13, C["dim"]))
    if note and ":" not in note:
        out.append(f'<g class="note-btn" data-note="{note}"><text x="{x + w - 16}" y="{y + h / 2 + 4}" text-anchor="middle" font-family="{FAM}" font-size="14" fill="#7ab8ff">ⓘ</text></g>')
    return out

def materialize(key, cols, found):
    new_cols = []
    for (cname, items) in cols:
        new_items = []
        for it in items:
            lst = list(it)
            code_idx = 3 if found else 5
            have = len(lst) > code_idx and lst[code_idx]
            if not have:
                extra = CODE_EXTRA.get(f"{key}|{lst[0]}")
                if extra:
                    while len(lst) <= code_idx:
                        lst.append(None)
                    lst[code_idx] = extra
            new_items.append(tuple(lst))
        new_cols.append((cname, new_items))
    return new_cols

def sub_svg(key, title, cols, found=True):
    cols = materialize(key, cols, found)
    CW, GAP = 470, 24
    per_row = 3
    has_code = any(len(it) > (3 if found else 5) and it[3 if found else 5] for c in cols for it in c[1])
    step = 64 if has_code else 56
    col_h = lambda items: 52 + 8 + len(items) * step + 16
    rows = [cols[i:i + per_row] for i in range(0, len(cols), per_row)]
    row_hs = [max(col_h(c[1]) for c in r) for r in rows]
    W = 70 + per_row * (CW + GAP) + 10
    H_ = 116 + sum(row_hs) + 20 * len(rows) + 24
    fill, border = GROUP_COLORS.get(key, GROUP_COLORS["deep"])
    P = title_bar(W, title)
    P.append(stext(72, 58, "●", 18, border))
    y = 126
    for r in rows:
        x = 50
        for ci, (cname, items) in enumerate(r):
            h = row_hs[0]
            P.append(srect(x, y, CW, h, C["card"], C["card_border"], 1.5))
            P.append(srect(x + 10, y + 12, CW - 20, 34, C["col_head"], C["col_border"], 1.5, 8))
            P.append(sltext(x + 24, y + 30, cname, 19, C["accent_t"], bold=True))
            iy = y + 60
            for ii, it in enumerate(items):
                name, detail = it[0], it[1]
                src = it[2] if len(it) > 2 else None
                code = it[3] if len(it) > 3 else None
                target = it[4] if len(it) > 4 else None
                note = it[5] if len(it) > 5 else None
                ih = 58 if code else 44
                if not note and not target and not src:
                    note = f"{key}:{ci}:{ii}"
                P.extend(item_box(x + 10, iy, CW - 20, name, detail, target, src, code, ih, note))
                iy += (ih + 6)
            x += CW + GAP
        y += row_hs[0] + 20
    return build_svg(W, H_, P)

def roadmap_svg():
    W, H_ = 1500, 130 + len(ROADMAP) * 190 + 40
    P = title_bar(W, "MCU 嵌入式学习路线图")
    y = 120
    for i, (stage, goal, items, verify, target) in enumerate(ROADMAP):
        extra = f' class="cbox" data-target="{target}"' if target else ""
        P.append(srect(60, y, W - 120, 170, C["card"], "#3d4754" if target else C["card_border"], 1.5, 12, extra=extra))
        if target:
            P.append(f"<title>点击进入：{SUB_NAMES[target]} 子知识结构图</title>")
        P.append(sltext(90, y + 32, stage, 22, "#7ab8ff", bold=True))
        P.append(sltext(90, y + 60, "目标：" + goal, 16, C["text"]))
        P.append(sltext(90, y + 88, "内容：" + " ｜ ".join(items), 14, C["dim"]))
        P.append(sltext(90, y + 116, "✅ 验证：" + verify, 14, "#8ce0ac"))
        if target:
            P.append(sltext(W - 110, y + 150, "进入 →", 14, "#7ab8ff"))
        if i < len(ROADMAP) - 1:
            P.append(f'<line x1="{W/2}" y1="{y+172}" x2="{W/2}" y2="{y+188}" stroke="#4a5564" stroke-width="3"/>')
        y += 190
    return build_svg(W, H_, P)

def resources_svg():
    cols = list(RES.items())
    CW, GAP = 560, 30
    per_row = 2
    rows = [cols[i:i + per_row] for i in range(0, len(cols), per_row)]
    col_h = lambda items: 52 + len(items) * 54 + 20
    row_hs = [max(col_h(c[1]) for c in r) for r in rows]
    W = 70 + per_row * (CW + GAP) + 10
    H_ = 116 + sum(row_hs) + 20 * len(rows) + 24
    P = title_bar(W, "学习资源金矿 · 误区纠正 · 面试高频")
    y = 126
    for r in rows:
        x = 50
        for (cname, items) in r:
            h = row_hs[0]
            P.append(srect(x, y, CW, h, C["card"], C["card_border"], 1.5))
            P.append(srect(x + 10, y + 12, CW - 20, 34, C["col_head"], C["col_border"], 1.5, 8))
            P.append(sltext(x + 24, y + 30, cname, 19, C["accent_t"], bold=True))
            iy = y + 60
            for it in items:
                if isinstance(it, tuple):
                    if len(it) == 3:
                        n, d, url = it
                        P.append(srect(x + 10, iy, CW - 20, 44, C["item"], "#3d4754", 1.5, 8, extra=f' class="cbox" data-src="{url}"'))
                        P.append(sltext(x + 24, iy + 18, "📄 " + n, 15.5, "#8ce0ac", bold=True))
                    else:
                        n, d = it
                        P.append(srect(x + 10, iy, CW - 20, 44, C["item"], C["item_border"], 1.5, 8))
                        P.append(sltext(x + 24, iy + 18, n, 16, C["text"], bold=True))
                    P.append(sltext(x + 24, iy + 36, d, 13, C["dim"]))
                else:
                    P.append(srect(x + 10, iy, CW - 20, 44, "#2a2438", "#6c3483", 1.5, 8))
                    P.append(sltext(x + 24, iy + 27, "★ " + it, 15.5, "#cf9ff2"))
                iy += 54
            x += CW + GAP
        y += row_hs[0] + 20
    return build_svg(W, H_, P)

# 总览分组：CATS = [(组名, [(key, 标题, 描述), ...]), ...]
CATS = []

def overview_svg():
    ncat = len(CATS)
    W = 2600
    H_ = 240 + ncat * 300 + 40
    P = [stext(W / 2, 46, "MCU 嵌入式知识结构图（全量版）", 44, "#FFFFFF", bold=True),
         stext(W / 2, 92, "六大领域 28 子系统 · 点击彩色框图进入子图 · 滚轮缩放 · 拖拽平移 · Esc 返回", 19, C["dim"])]
    y = 130
    for ci, (gname, items) in enumerate(CATS):
        n = len(items)
        cols = 4
        rows = (n + cols - 1) // cols
        GH = 46 + rows * 118 + 30
        P.append(srect(70, y, W - 140, GH, "#171b1f", C["layer_border"], 2.5, 16))
        P.append(sltext(95, y + 34, gname, 25, C["accent_t"], bold=True))
        for i, (key, t, d) in enumerate(items):
            col, row = i % cols, i // cols
            x = 95 + col * 610
            yy = y + 56 + row * 118
            fill, border = GROUP_COLORS.get(key, GROUP_COLORS["deep"])
            P.append(srect(x, yy, 580, 100, fill, border, 2.5, extra=f' class="cbox" data-target="{key}"'))
            P.append(f"<title>点击进入：{t}</title>")
            P.append(sltext(x + 20, yy + 34, t + "  ▶", 21, "#FFFFFF", bold=True))
            P.append(sltext(x + 20, yy + 66, d, 14.5, "#EAF2FC"))
            P.append(sltext(x + 540, yy + 80, "→", 16, "#FFFFFF"))
        y += GH + 22
    return build_svg(W, H_, P)

CATS = [
    ("内核与架构", [("mcu_arch", "Cortex-M 架构", "模式/异常/寄存器/MPU/FPU"), ("boot", "启动与存储器", "启动流程/Flash/SRAM/分散加载"), ("clock", "时钟系统", "时钟树/PLL/低功耗模式"), ("nvic", "中断与异常", "NVIC/优先级/HardFault"), ("mem_bus", "存储器与总线", "FMC/QSPI/位带/缓存")]),
    ("外设控制", [("gpio", "GPIO / EXTI", "模式/开漏/外部中断"), ("timer", "定时器", "PWM/捕获/编码器"), ("pwm_motor", "电机与 PWM", "有刷/BLDC/步进/PID"), ("dma", "DMA", "外设联动/双缓冲"), ("adc", "ADC / DAC", "采样/DMA 采集/滤波"), ("audio", "音频接口", "I2S/编解码器/播放录音")]),
    ("通信总线", [("uart", "UART / RS485", "串口/Modbus/协议设计"), ("spi", "SPI 总线", "时序/Flash/OLED/QSPI"), ("i2c", "I2C 总线", "时序/EEPROM/死锁恢复"), ("can", "CAN 总线", "仲裁/滤波/CAN FD"), ("usb", "USB 接口", "枚举/CDC/描述符"), ("eth", "以太网", "PHY/LwIP/MQTT"), ("wireless", "无线通信", "BLE/WiFi/LoRa/NB-IoT")]),
    ("系统与软件", [("rtos", "FreeRTOS 内核", "任务/调度/通信"), ("rtos_adv", "RTOS 进阶", "tickless/SMP/实时性"), ("memory_mgmt", "内存与数据结构", "环形缓冲/内存池"), ("software_arch", "架构与模式", "分层/状态机/事件驱动"), ("lowpower", "低功耗可靠性", "模式/看门狗/IAP"), ("storage_fs", "存储与文件系统", "SD 卡/FATFS/LittleFS"), ("industry", "行业应用", "汽车/工业/IoT/消费")]),
    ("工程与调试", [("toolchain", "工具链构建", "Keil/GCC/CubeMX/CI"), ("debug_adv", "调试技术", "ITM/SWO/RTT/性能"), ("reliability", "可靠性与 EMC", "ESD/看门狗/MISRA"), ("security", "嵌入式安全", "加密/安全启动/防抄板"), ("testing", "软件测试", "Unity/HIL/静态分析")]),
    ("硬件与联调", [("electronics", "电子电路", "电平/电源/信号调理"), ("sensors", "传感器与显示", "IMU/OLED/人机交互"), ("gui", "图形界面", "LVGL/TouchGFX/字库"), ("pcb", "PCB 与联调", "布局/晶振/信号完整性")]),
]
OVERVIEW_SVG = overview_svg()

CSS = """
:root { --bg:#121418; --card:#1d2126; --border:#2a3038; --border2:#333b46; --text:#e8edf3; --dim:#93a0b0; --accent:#4f8cff; }
* { box-sizing:border-box; }
body { margin:0; font-family:"Microsoft YaHei","PingFang SC",sans-serif; color:var(--text);
  background:radial-gradient(1100px 500px at 50% -8%, #1c2740 0%, var(--bg) 55%); min-height:100vh; }
header { position:sticky; top:0; z-index:50; display:flex; align-items:center; gap:12px; padding:11px 26px;
  background:rgba(18,20,24,.85); backdrop-filter:blur(12px); border-bottom:1px solid var(--border); flex-wrap:wrap; }
header .logo { font-size:21px; font-weight:800; background:linear-gradient(90deg,#7ab8ff,#e8edf3);
  -webkit-background-clip:text; background-clip:text; color:transparent; white-space:nowrap; }
header .crumb { color:var(--dim); font-size:13px; }
header .crumb a { color:var(--accent); text-decoration:none; cursor:pointer; }
header .crumb b { color:var(--text); }
header .spacer { flex:1; }
.btn { border:1px solid var(--border2); background:#242a32; color:var(--text); border-radius:10px; padding:7px 15px;
  font-size:13px; cursor:pointer; transition:all .15s; white-space:nowrap; }
.btn:hover { background:#2d3540; border-color:var(--accent); color:#fff; }
.btn.primary { background:var(--accent); border-color:var(--accent); color:#fff; font-weight:700; }
.btn.primary:hover { filter:brightness(1.12); }
.btn.gold { border-color:#8a6d1f; color:#f0d98c; }
main { max-width:1720px; margin:0 auto; padding:20px 24px 40px; }
.hero { text-align:center; margin:6px 0 18px; }
.hero h1 { margin:0; font-size:33px; font-weight:800; letter-spacing:1px;
  background:linear-gradient(90deg,#7ab8ff 0%,#b8d8ff 45%,#e8edf3 100%); -webkit-background-clip:text; background-clip:text; color:transparent; }
.hero p { margin:8px 0 0; color:var(--dim); font-size:14px; }
.stats { display:flex; gap:10px; justify-content:center; margin:14px 0 4px; flex-wrap:wrap; }
.stat { background:var(--card); border:1px solid var(--border); border-radius:999px; padding:5px 16px; font-size:12.5px; color:var(--dim); }
.stat b { color:var(--accent); }
.legend { display:flex; gap:8px; flex-wrap:wrap; justify-content:center; margin:12px 0 16px; }
.chip { display:inline-flex; align-items:center; gap:7px; background:var(--card); border:1px solid var(--border);
  border-radius:999px; padding:5px 13px; font-size:12.5px; color:var(--dim); cursor:pointer; transition:all .15s; user-select:none; }
.chip:hover { border-color:var(--accent); color:var(--text); transform:translateY(-1px); }
.chip i { width:10px; height:10px; border-radius:3px; display:inline-block; }
.card { background:var(--card); border:1px solid var(--border); border-radius:14px; box-shadow:0 10px 30px rgba(0,0,0,.35); padding:16px; }
.stage { position:relative; overflow:hidden; border:1px solid var(--border); border-radius:10px; background:#15181c; cursor:grab; height:76vh; }
.stage.dragging { cursor:grabbing; }
.inner { transform-origin:0 0; }
.inner svg { display:block; max-width:none; }
.cbox { cursor:pointer; transition:filter .15s; }
.cbox:hover { filter:brightness(1.22) drop-shadow(0 0 10px rgba(120,180,255,.35)); }
.note-btn { cursor:pointer; }
.note-btn:hover text { fill:#ffffff; }
.drawer { position:fixed; top:0; right:0; width:460px; max-width:94vw; height:100vh; z-index:200;
  background:#171b20; border-left:1px solid #2c333d; box-shadow:-14px 0 44px rgba(0,0,0,.55);
  transform:translateX(105%); transition:transform .22s ease; display:flex; flex-direction:column; }
.drawer.open { transform:translateX(0); }
.drawer-head { display:flex; align-items:center; justify-content:space-between; padding:14px 18px;
  border-bottom:1px solid #2c333d; background:#1a1e24; }
.drawer-head b { color:#7ab8ff; font-size:16px; }
.drawer-head button { background:none; border:none; color:#93a0b0; font-size:18px; cursor:pointer; padding:4px 8px; }
.drawer-head button:hover { color:#fff; }
#d-body { padding:16px 18px; overflow-y:auto; flex:1; font-size:14px; line-height:1.75; }
.ndet { color:#93a0b0; margin-bottom:10px; }
.ncode { background:#121418; border:1px solid #2c333d; border-radius:8px; padding:10px 12px; color:#f0d98c;
  font-family:Consolas,monospace; font-size:12.5px; overflow-x:auto; white-space:pre-wrap; margin:8px 0; }
.ntext { color:#e8edf3; }
.nsec { color:#9fc3ff; font-weight:700; margin:14px 0 6px; font-size:13px; }
.nlink { display:block; color:#7ab8ff; text-decoration:none; padding:7px 12px; margin:5px 0;
  background:#242a32; border:1px solid #2c333d; border-radius:8px; font-size:13px; }
.nlink:hover { border-color:#4f8cff; background:#2a323d; }
footer { text-align:center; color:#5d6875; font-size:12px; margin:24px 0 8px; }
.subview, #view-overview { animation:fadein .18s ease; }
@keyframes fadein { from { opacity:0; transform:translateY(4px); } to { opacity:1; transform:none; } }
"""

JS = r"""
function attachZoom(stageId, svgW, svgH){
  var stage = document.getElementById(stageId);
  var inner = stage.querySelector('.inner');
  var scale = 1, tx = 0, ty = 0;
  function apply(){ inner.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + scale + ')'; }
  function zoomAt(mx, my, f){
    var ns = Math.max(0.1, Math.min(8, scale * f));
    tx = mx - (mx - tx) * (ns / scale);
    ty = my - (my - ty) * (ns / scale);
    scale = ns; apply();
  }
  stage.zoomBy = function(f){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, f); };
  stage.setZoom = function(s){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, s/scale); };
  stage.fit = function(){
    var r = stage.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) { stage._needFit = true; return; }
    stage._needFit = false;
    var s = Math.min(r.width / svgW, r.height / svgH, 1);
    scale = s; tx = (r.width - svgW * s) / 2; ty = (r.height - svgH * s) / 2; apply();
  };
  stage.addEventListener('wheel', function(e){
    e.preventDefault();
    var r = stage.getBoundingClientRect();
    zoomAt(e.clientX - r.left, e.clientY - r.top, e.deltaY < 0 ? 1.15 : 1/1.15);
  }, { passive: false });
  var drag = null;
  stage.addEventListener('mousedown', function(e){
    drag = { x: e.clientX, y: e.clientY, tx: tx, ty: ty };
    stage.classList.add('dragging'); e.preventDefault();
  });
  window.addEventListener('mousemove', function(e){
    if (!drag) return;
    tx = drag.tx + e.clientX - drag.x; ty = drag.ty + e.clientY - drag.y; apply();
  });
  window.addEventListener('mouseup', function(){ drag = null; stage.classList.remove('dragging'); });
  stage.fit();
  return stage;
}
var SUB_NAMES = %SUBNAMES%;
var STACK = ['overview'];
function current(){ return STACK[STACK.length - 1]; }
function showView(key){
  if (key === current()) return;
  STACK.push(key); render();
}
function showSub(key){ showView(key); }
function back(){ if (STACK.length > 1) { STACK.pop(); render(); } }
function showOverview(){ STACK = ['overview']; render(); }
function showAt(i){ STACK = STACK.slice(0, i + 1); render(); }
function render(){
  document.querySelectorAll('.subview, #view-overview').forEach(function(v){ v.style.display = 'none'; });
  var cur = current();
  var el = cur === 'overview' ? document.getElementById('view-overview') : document.getElementById('view-' + cur);
  if (el) el.style.display = 'block';
  var st = document.getElementById('stage-' + cur);
  if (st && st._needFit) st.fit();
  var html = STACK.map(function(k, i){
    var n = k === 'overview' ? '总览' : (SUB_NAMES[k] || k);
    return i === STACK.length - 1 ? '<b>' + n + '</b>' : '<a onclick="showAt(' + i + ')">' + n + '</a>';
  }).join(' / ');
  document.getElementById('crumb').innerHTML = html;
  document.getElementById('btn-back').style.display = STACK.length > 1 ? '' : 'none';
}
document.addEventListener('keydown', function(e){
  if (e.key === 'Escape') {
    if (document.getElementById('drawer').classList.contains('open')) { closeNote(); } else { showOverview(); }
  }
});
document.querySelectorAll('.cbox').forEach(function(r){
  r.addEventListener('click', function(){
    var tgt = r.getAttribute('data-target');
    if (tgt) { showView(tgt); return; }
    var src = r.getAttribute('data-src');
    if (src) { window.open(src, '_blank'); return; }
    openNote(r.getAttribute('data-note'));
  });
});
document.querySelectorAll('.note-btn').forEach(function(b){
  b.addEventListener('click', function(e){ e.stopPropagation(); openNote(b.getAttribute('data-note')); });
});
var NOTES = %NOTES%;
function openNote(key){
  var el = document.querySelector('[data-note="' + key + '"]');
  var n = NOTES[key];
  var name = el ? el.getAttribute('data-name') : (n && n.title ? n.title : key);
  var det = el ? el.getAttribute('data-detail') : '';
  var code = el ? el.getAttribute('data-code') : '';
  var sym = el ? el.getAttribute('data-sym') : '';
  var html = '';
  if (det) html += '<div class="ndet">' + det + '</div>';
  if (code) html += '<pre class="ncode">' + code + '</pre>';
  if (sym) html += '<a class="nlink" target="_blank" href="https://elixir.bootlin.com/linux/latest/C/ident/' + sym + '">📌 同名符号检索（跨内核参考）</a>';
  if (n && n.text) html += '<div class="ntext">' + n.text + '</div>';
  if (n && n.links) html += '<div class="nsec">📖 精选资源</div>' + n.links.map(function(l){
    return '<a class="nlink" target="_blank" href="' + l[1] + '">' + l[0] + '</a>';
  }).join('');
  var kw = encodeURIComponent((name || key) + ' STM32 嵌入式');
  html += '<div class="nsec">🔎 更多资料（自动检索）</div>';
  html += '<a class="nlink" target="_blank" href="https://cn.bing.com/search?q=' + kw + '">🔍 Bing 搜索文章</a>';
  html += '<a class="nlink" target="_blank" href="https://search.bilibili.com/all?keyword=' + kw + '">🎬 B站 视频教程</a>';
  html += '<a class="nlink" target="_blank" href="https://www.freertos.org/Documentation/">📘 FreeRTOS 文档</a>';
  document.getElementById('d-title').textContent = name;
  document.getElementById('d-body').innerHTML = html;
  document.getElementById('drawer').classList.add('open');
}
function closeNote(){ document.getElementById('drawer').classList.remove('open'); }
"""

JS_TAIL = r"""
var STAGE_SIZES = %SIZES%;
Object.keys(STAGE_SIZES).forEach(function(k){
  var s = STAGE_SIZES[k];
  attachZoom('stage-' + k, s[0], s[1]);
});
render();
"""

legend_html = "".join(f'<span class="chip" onclick="showSub(\'{k}\')"><i style="background:{GROUP_COLORS[k][0]}"></i>{n}</span>' for k, n in [(k, v[0]) for k, v in SUBS.items()])
views = [f'<div id="view-overview"><div class="stage" id="stage-overview"><div class="inner" id="inner-overview">\n{OVERVIEW_SVG}\n</div></div></div>']
for key, (title, cols) in {**SUBS, **DEEP}.items():
    svg = sub_svg(key, title, cols, found=True)
    views.append(f"<div class='subview' id='view-{key}' style='display:none'><div class='stage' id='stage-{key}'><div class='inner' id='inner-{key}'>\n{svg}\n</div></div></div>")
views.append(f"<div class='subview' id='view-roadmap' style='display:none'><div class='stage' id='stage-roadmap'><div class='inner' id='inner-roadmap'>\n{roadmap_svg()}\n</div></div></div>")
views.append(f"<div class='subview' id='view-resources' style='display:none'><div class='stage' id='stage-resources'><div class='inner' id='inner-resources'>\n{resources_svg()}\n</div></div></div>")

js = JS.replace("%SUBNAMES%", str(SUB_NAMES))
js = js.replace("%NOTES%", json.dumps(NOTES, ensure_ascii=False))

html_doc = f"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>MCU 嵌入式知识结构图（全量版）</title>
<style>{CSS}</style>
</head>
<body>
<header>
  <span class="logo">⚡ MCU Map</span>
  <span class="crumb" id="crumb"><b>总览</b></span>
  <span class="spacer"></span>
  <button class="btn gold" onclick="showView('roadmap')">🚀 学习路线</button>
  <button class="btn gold" onclick="showView('resources')">📚 资源误区</button>
  <button class="btn primary" id="btn-back" onclick="back()" style="display:none">← 返回</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).zoomBy(1.3)">＋</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).zoomBy(1/1.3)">－</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).setZoom(1)">100%</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).fit()">适应</button>
</header>
<main>
  <div class="hero">
    <h1>MCU 嵌入式知识结构图（全量版）</h1>
    <p>六大领域 {len(SUBS)} 子系统 + {len(DEEP)} 专题 · 点击框图进入 · ⓘ 精讲 · 普通条目点击弹讲解 · Esc 返回</p>
    <div class="stats">
      <span class="stat">子系统图 <b>{len(SUBS)}</b></span>
      <span class="stat">专题深挖 <b>{len(DEEP)}</b></span>
      <span class="stat">代码实例 <b>100+</b></span>
      <span class="stat">精讲 <b>{len(NOTES)}</b></span>
    </div>
    <div class="legend">{legend_html}
      <span class="chip" onclick="showView('roadmap')" style="border-color:#8a6d1f;color:#f0d98c">🚀 学习路线</span>
      <span class="chip" onclick="showView('resources')" style="border-color:#6c3483;color:#cf9ff2">📚 资源误区</span>
    </div>
  </div>
  <div class="card">
    {''.join(views)}
  </div>
  <footer>MCU 嵌入式知识结构 · 全量版 v2 · {1 + len(SUBS) + len(DEEP) + 2} 视图 · 江协科技 / FreeRTOS 官方文档 / STM32 手册合集</footer>
</main>
<div id="drawer" class="drawer">
  <div class="drawer-head"><b id="d-title">讲解</b><button onclick="closeNote()">✕</button></div>
  <div id="d-body"></div>
</div>
<script>
{js}
</script>
<script>
{JS_TAIL}
</script>
</body>
</html>
"""

sizes = {"overview": [int(OVERVIEW_SVG.split('width="')[1].split('"')[0]), int(OVERVIEW_SVG.split('height="')[1].split('"')[0])]}
r_svg = roadmap_svg()
sizes["roadmap"] = [int(r_svg.split('width="')[1].split('"')[0]), int(r_svg.split('height="')[1].split('"')[0])]
res_svg = resources_svg()
sizes["resources"] = [int(res_svg.split('width="')[1].split('"')[0]), int(res_svg.split('height="')[1].split('"')[0])]
for key, v in {**SUBS, **DEEP}.items():
    svg = sub_svg(key, *v, found=True)
    sizes[key] = [int(svg.split('width="')[1].split('"')[0]), int(svg.split('height="')[1].split('"')[0])]
html_doc = html_doc.replace("%SIZES%", str(sizes))

out = r"D:\GIT-SPACE\D00\_notes\mcu_structure_interactive.html"
with open(out, "w", encoding="utf-8") as f:
    f.write(html_doc)
print("html:", out)
print("subs:", len(SUBS), "deep:", len(DEEP), "roadmap:", len(ROADMAP), "notes:", len(NOTES), "views:", 1 + len(SUBS) + len(DEEP) + 2)
