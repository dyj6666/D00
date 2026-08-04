# ARM GCC 交叉编译工具链（STM32F4）——与 APP 工程保持一致
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 优先使用 STM32CubeCLT 自带的工具链，其次取 PATH 中的 arm-none-eabi-gcc
set(ARM_TOOLCHAIN_ROOT "D:/STM32CUBECLT/STM32CubeCLT_1.18.0/GNU-tools-for-STM32/bin")

if(EXISTS "${ARM_TOOLCHAIN_ROOT}/arm-none-eabi-gcc.exe")
    set(CMAKE_C_COMPILER   "${ARM_TOOLCHAIN_ROOT}/arm-none-eabi-gcc.exe")
    set(CMAKE_ASM_COMPILER "${ARM_TOOLCHAIN_ROOT}/arm-none-eabi-gcc.exe")
    set(CMAKE_OBJCOPY      "${ARM_TOOLCHAIN_ROOT}/arm-none-eabi-objcopy.exe")
else()
    find_program(CMAKE_C_COMPILER   NAMES arm-none-eabi-gcc)
    find_program(CMAKE_ASM_COMPILER NAMES arm-none-eabi-gcc)
    find_program(CMAKE_OBJCOPY      NAMES arm-none-eabi-objcopy)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
