# ================================================================
# lvgl —— LVGL 8.3.5（vendored，纯可移植 C）
#
# 说明：
#   - 源码来自 D00 工程的 APP/APP/Middlewares/Third_Party/lvgl（与 STM32 侧同版本），
#     离线可用、不依赖组件注册表；
#   - lv_conf.h 放在本组件根目录，LVGL 通过 __has_include("lv_conf.h") 自动引入；
#   - 只编译 src/ 下的实现，排除 demos/examples（本项目自带 UI）。
# ================================================================
file(GLOB_RECURSE LVGL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)

idf_component_register(
    SRCS ${LVGL_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)

# 让所有使用 lvgl 的组件都能包含 lvgl.h / lv_conf.h
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)

# LVGL 官方推荐：性能优先的编译选项
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
)
