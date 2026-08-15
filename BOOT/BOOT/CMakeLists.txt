cmake_minimum_required(VERSION 3.16)
project(BOOT C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(PYTHON_EXECUTABLE NAMES python python3)

if(CMAKE_CROSSCOMPILING)
    # ============================================================
    # BOOT 固件目标（arm-none-eabi-gcc）
    #   构建：cmake -S . -B build-fw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-stm32f4.cmake
    # ============================================================
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/Core/Startup/BOOT.ld)

    set(FW_SOURCES
        Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f407xx.s
        Core/Src/main.c
        Core/Src/gpio.c
        Core/Src/dma.c
        Core/Src/iwdg.c
        Core/Src/rtc.c
        Core/Src/usart.c
        Core/Src/stm32f4xx_it.c
        Core/Src/stm32f4xx_hal_msp.c
        Core/Src/system_stm32f4xx.c
        BootApp/boot_app.c
        BootServices/boot_err.c
        BootServices/ymodem.c
        BootServices/ymodem_port.c
        BootServices/flash_if.c
    BootServices/boot_param.c
    BootServices/security.c
    BootServices/esp_flash.c
    BootServices/ota_source.c
    BootServices/ota_backup.c
    BootServices/aes.c
        BootServices/my_sha256.c
        BootServices/uECC.c
        BootServices/crc32.c
        BootServices/fifo.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_iwdg.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
    )

    set(FW_INCLUDES
        Core/Inc
        Config
        BootApp
        BootServices
        Drivers
        Drivers/CMSIS/Include
        Drivers/CMSIS/Device/ST/STM32F4xx/Include
        Drivers/STM32F4xx_HAL_Driver/Inc
        Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    )

    set(FW_FLAGS
        -mcpu=cortex-m4
        -mthumb
        -mfpu=fpv4-sp-d16
        -mfloat-abi=hard
        -Os
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unknown-pragmas
    )

    add_executable(firmware.elf ${FW_SOURCES})
    target_include_directories(firmware.elf PRIVATE ${FW_INCLUDES})
    target_compile_definitions(firmware.elf PRIVATE STM32F407xx USE_HAL_DRIVER AES256=1)
    target_compile_options(firmware.elf PRIVATE ${FW_FLAGS})
    target_link_options(firmware.elf PRIVATE
        -mcpu=cortex-m4
        -mthumb
        -mfpu=fpv4-sp-d16
        -mfloat-abi=hard
        -T ${LINKER_SCRIPT}
        -Wl,--gc-sections
        -Wl,-Map=${CMAKE_BINARY_DIR}/firmware.map
        --specs=nano.specs
        --specs=nosys.specs
    )

    add_custom_command(TARGET firmware.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
                $<TARGET_FILE:firmware.elf>
                ${CMAKE_BINARY_DIR}/BOOT.bin
        COMMENT "Generate BOOT.bin"
    )
else()
    # ============================================================
    # 主机单元测试（纯逻辑服务层）
    # ============================================================
    add_executable(host_tests
        tests/test_main.c
        tests/test_services.c
        tests/test_ymodem.c
        BootServices/crc32.c
        BootServices/fifo.c
        BootServices/ymodem.c
    )

    target_include_directories(host_tests PRIVATE
        BootServices
        Config
        tests
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(host_tests PRIVATE -Wall -Wextra)
    endif()

    enable_testing()
    add_test(NAME host_tests COMMAND host_tests)
endif()
