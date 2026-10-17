# ================================================================
# ui —— LVGL 端口层 + 应用界面（仪表盘）
#
# 职责：
#   lvgl_port.c     LVGL 初始化/时基/显示刷新(双缓冲+异步 DMA 重叠)/触摸输入/日志转发/刷新任务
#   ui_dashboard.c  应用界面：三页（概览/姿态/系统）+ 页面切换动画 + 实时数据
#
# 依赖：lvgl（移植核心）、bsp（面板与触摸）、esp_timer（LVGL 时基）
# ================================================================
idf_component_register(
    SRCS
        "src/ui_port.c"
        "src/ui_dashboard.c"
        "src/img_ldd.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl
        bsp
        esp_timer
        esp_lcd
)
