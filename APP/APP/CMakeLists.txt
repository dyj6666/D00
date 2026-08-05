cmake_minimum_required(VERSION 3.16)
project(APP C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(PYTHON_EXECUTABLE NAMES python python3)

if(CMAKE_CROSSCOMPILING)
    # ============================================================
    # 固件目标（arm-none-eabi-gcc）
    #   构建：cmake -S . -B build-fw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-stm32f4.cmake
    # ============================================================
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/Core/Startup/STM32F407ZGTx_APP.ld)

    set(FW_SOURCES
        # 启动与核心
        Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f407xx.s
        Core/Src/main.c
        Core/Src/freertos.c
        Core/Src/stm32f4xx_it.c
        Core/Src/stm32f4xx_hal_msp.c
        Core/Src/stm32f4xx_hal_timebase_tim.c
        Core/Src/system_stm32f4xx.c
        # 外设初始化
        Core/Src/gpio.c
        Core/Src/dma.c
        Core/Src/fsmc.c
        Core/Src/iwdg.c
        Core/Src/rtc.c
        Core/Src/tim.c
        Core/Src/usart.c
        # BSP 抽象层
        BSP/bsp_system.c
        BSP/bsp_uart.c
        BSP/bsp_watchdog.c
        BSP/bsp_rtc.c
        BSP/bsp_gpio.c
        # 系统服务
        SystemServices/logger.c
        SystemServices/shell.c
        SystemServices/event_bus.c
        SystemServices/module.c
        SystemServices/sysmon.c
        SystemServices/var_manager.c
        SystemServices/var_list.c
        SystemServices/data_link.c
        SystemServices/crc16.c
        SystemServices/protocol.c
        SystemServices/watchdog.c
        SystemServices/la_sample.c
        SystemServices/la_buffer.c
        SystemServices/la_trigger.c
        SystemServices/signal_gen.c
        # 应用
        Application/key_app.c
        Application/led_app.c
        Application/ota_agent.c
        Application/data_agent.c
        # FreeRTOS
        Middlewares/Third_Party/FreeRTOS/Source/croutine.c
        Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
        Middlewares/Third_Party/FreeRTOS/Source/list.c
        Middlewares/Third_Party/FreeRTOS/Source/queue.c
        Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
        Middlewares/Third_Party/FreeRTOS/Source/tasks.c
        Middlewares/Third_Party/FreeRTOS/Source/timers.c
        Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c
        Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
        Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c
        # STM32 HAL
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
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_fsmc.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_nor.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sram.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_nand.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pccard.c
    )

    set(FW_INCLUDES
        Core/Inc
        Config
        BSP
        SystemServices
        Application
        Drivers
        Drivers/CMSIS/Include
        Drivers/CMSIS/Device/ST/STM32F4xx/Include
        Drivers/STM32F4xx_HAL_Driver/Inc
        Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
        Middlewares/Third_Party/FreeRTOS/Source/include
        Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
        Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    )

    set(FW_FLAGS
        -mcpu=cortex-m4
        -mthumb
        -mfpu=fpv4-sp-d16
        -mfloat-abi=hard
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
        -Wno-unused-parameter
    )

    add_executable(firmware.elf ${FW_SOURCES})
    target_include_directories(firmware.elf PRIVATE ${FW_INCLUDES})
    target_compile_definitions(firmware.elf PRIVATE STM32F407xx USE_HAL_DRIVER)
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
                ${CMAKE_BINARY_DIR}/APP.bin
        COMMENT "Generate APP.bin"
    )
    if(PYTHON_EXECUTABLE)
        add_custom_command(TARGET firmware.elf POST_BUILD
            COMMAND ${PYTHON_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Script/append_app_magic.py
                    ${CMAKE_BINARY_DIR}/APP.bin
                    --out ${CMAKE_BINARY_DIR}/APP_flash.bin
            COMMENT "Generate APP_flash.bin (with validity magic)"
        )
    endif()
else()
    # ============================================================
    # 主机单元测试（纯逻辑层）
    # ============================================================
    add_executable(host_tests
        tests/test_protocol.c
        SystemServices/protocol.c
        SystemServices/crc16.c
        SystemServices/var_list.c
    )

    target_include_directories(host_tests PRIVATE
        SystemServices
        Config
        tests
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(host_tests PRIVATE -Wall -Wextra)
    endif()

    enable_testing()
    add_test(NAME host_tests COMMAND host_tests)
endif()
