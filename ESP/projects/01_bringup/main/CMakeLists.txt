# bringup 应用组件：入口 + 业务(app)
#
# 注意：这里故意不写 REQUIRES/PRIV_REQUIRES。
# IDF 只在 main 组件"没有手工声明依赖"时，才让它自动依赖本次构建里的所有组件
# （见 tools/cmake/project.cmake 的 "Special treatment for 'main' component"）。
# 一旦写了 REQUIRES，main 就只剩列出来的那几个组件，esp_flash.h / esp_psram.h
# 这类头文件会立刻找不到。
#
# 板级驱动已经独立成 components/bsp 组件（它自己显式声明了 REQUIRES），
# main 作为"依赖全部组件"的特殊组件自动就能用上，无需在这里列出来。
idf_component_register(SRC_DIRS "." "app"
                       INCLUDE_DIRS "." "app")
