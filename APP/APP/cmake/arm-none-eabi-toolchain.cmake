# STM32CubeCLT 交叉编译工具链
# 用法: cmake -S . -B build-cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 交叉编译时不做可执行链接探测
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_ASM_COMPILER_FORCED TRUE)
