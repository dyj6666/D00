/* ================================================================
 * app_config —— APP 全局配置（分区表/OTA/日志/Shell/事件总线/HOSTLINK）
 *
 * 架构位置：APP 配置层；所有业务模块通过本头文件取参数
 * 关键约束：分区地址与 BOOT/boot_config.h 严格一致，改动必须同步两侧
 * ================================================================ */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ================================================================
 * 显示架构主控：LVGL 顶级 GUI 作为核心链接（方案B 后启用）
 *   1 = LVGL 独占屏幕（gui_app），旧自绘页面层（lcd_app/lcd_ui）
 *       仅保留接口兼容（触摸校准/测试命令不绘制），代码留待后续
 *       彻底移除时清理；
 *   0 = 旧自绘页面层（兼容回退，不推荐）
 * ================================================================ */
#define LVGL_GUI_MASTER  1


#define DEBUG_APP 0   /* 调试开关：1=输出更多调试信息 */

#define BOOT_FLAG_UPGRADE  0x5A5A   /* BKP 寄存器升级触发标志 */

/* ---------------- OTA 分区（与 BOOT/boot_config.h 严格一致） ---------------- */
/* 方案B：内部 DOWNLOAD/BACKUP 已取消，RUN 区 832KB（0x08010000~0x080DFFFF）；
 * 下载暂存与回滚源均在外部 Flash（ota_dl / img_lib）。 */

/* 外部 Flash OTA 下载（方案A+B：下载区外移；固件上限 = RUN 区 832KB） */
#define OTA_EXT_DL_SIZE         (1024 * 1024)   /* 外部 ota_dl 单槽容量（1MB） */
#define OTA_EXT_DL_SAFE         (OTA_EXT_DL_SIZE - 1024)

/* 断点续传会话槽区：PARAM 扇区空余（0x080E2000，避开参数槽 +0/+1024）。
 * 32KB = 1024 槽 × 32B，覆盖 ≤245KB 固件；与 BOOT/boot_config.h 严格一致。 */
#define OTA_SESSION_BASE        0x080E2000UL
#define OTA_SESSION_SLOTS       1024
#define OTA_SESSION_MAGIC       0x4F54414DUL   /* 'OTAM' */

#define OTA_PARAM_ADDR          0x080E0000UL   /* 参数区 */
#define OTA_PARAM_SLOT_OFFSET   1024
#define OTA_PARAM_MAGIC         0x50524D54UL
#define OTA_APP_VERSION_ADDR    0x080DFFFCUL   /* RUN 区尾部版本号（方案B） */
#define OTA_STATE_NORMAL        0x00000001UL
#define OTA_STATE_PENDING       0x00000002UL
#define OTA_STATE_RECOVERY      0x00000003UL
#define OTA_STATE_UPGRADE       0x00000004UL

/* OTA 协议：单包最大数据块 */
#define OTA_CHUNK_MAX           240

/* ---------------- 日志系统 ---------------- */
#define LOG_TX_STREAM_SIZE      2048   /* 发送流缓冲：LOG 输出队列深度 */
#define LOG_RX_STREAM_SIZE      1024   /* 接收流缓冲：Shell 命令输入队列 */
#define LOG_RX_DMA_BUF_SIZE     256    /* UART RX DMA 缓冲 */
#define LOG_TX_DMA_CHUNK        128    /* TX DMA 单次搬运块 */

/* ---------------- Shell ---------------- */
#define SHELL_LINE_MAX          128    /* 单行命令最大长度 */
#define SHELL_CMD_MAX           20     /* 命令目录最大条目数 */

/* CAN1 总线（Shell + OTA 双通道）：BSP 驱动就绪，1Mbps；
 * 置 0 可整体关闭 CAN Shell 传输（OTA CAN 服务随 BSP 一起失效） */
#ifndef CMD_ENABLE_CAN
#define CMD_ENABLE_CAN          1
#endif

/* ---------------- 事件总线 ---------------- */
#define EVENT_BUS_SUBS_MAX      8     /* 每个消息类型最大订阅者数 */
#define EVENT_BUS_QUEUE_LENGTH  64    /* 主事件队列深度 */
#define EVENT_BUS_POOL_SIZE     32    /* 静态消息池槽位数 */
#define EVENT_BUS_MSG_MAX_PAYLOAD 128 /* 单条消息 payload 最大长度（字节） */

/* ---------------- 系统定时器 ---------------- */
#define SYS_TICK_1S_PERIOD_MS   1000
#define SYS_TICK_200MS_PERIOD_MS 200
#define KEY_SCAN_PERIOD_MS      10     /* 按键扫描周期 */

#define DEVICE_I2C_TIMEOUT_MS   100    /* I2C 设备访问超时 */

#define WDOG_FEED_PERIOD_MS     1000   /* 喂狗周期：须小于 IWDG 超时的一半 */

/* ---------------- 上位机通信 (DataLink) ---------------- */
#define HOSTLINK_RX_DMA_BUF_SIZE    256  /* DMA 接收缓冲 */
#define HOSTLINK_TX_QUEUE_LEN       8    /* TX 整帧队列深度（保帧边界，防大块截断） */
#define HOSTLINK_TX_FRAME_MAX       256  /* 单帧最大字节（含 CRC） */
#define HOSTLINK_TX_DMA_CHUNK       256  /* DMA 发送缓冲大小 */
#define HOSTLINK_CMD_QUEUE_LEN      16   /* 命令队列深度（突发帧不丢） */
#define HOSTLINK_MAX_VARS           64   /* 最大注册变量数 */
#define HOSTLINK_MAX_SUBSCRIBE      16   /* 最大订阅变量数 */
#define HOSTLINK_SAMPLE_PERIOD_MS   10   /* 默认采集周期 */
#define HOSTLINK_CRC_POLY           0xA001  /* CRC-16/MODBUS 多项式 */

/* 调试模式：1 = 关闭 IWDG 与任务级看门狗（供 gdb 断点调试，发布前务必置 0）。
 * 可用编译选项 -DAPP_DEBUG_MODE=1 覆盖（构建系统/CI 双模式验证）。 */
#ifndef APP_DEBUG_MODE
#define APP_DEBUG_MODE              1   /* 断电重启测试用调试构建（IWDG 关，死机取证友好） */
#endif

/* 崩溃注入测试命令（crash <bus|undef|stack|assert>）：
 * 跟随 APP_DEBUG_MODE：发布构建（0）自动禁用，避免固件含崩溃后门；
 * 开发调试用 -DAPP_DEBUG_MODE=1 重新启用。 */
#ifndef CRASH_INJECT_ENABLE
#define CRASH_INJECT_ENABLE         APP_DEBUG_MODE
#endif



#endif
