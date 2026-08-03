/*
 * FreeRTOS Kernel V10.3.1 compatible portmacro.h for GCC/ARM_CM4F.
 *
 * 此文件仅用于本仓库 Script/check_firmware_syntax.ps1 的交叉编译语法冒烟检查
 * （arm-none-eabi-gcc -fsyntax-only），内容与官方 FreeRTOS 内核
 * portable/GCC/ARM_CM4F/portmacro.h 等价。Keil 工程仍使用
 * Middlewares/.../portable/RVDS/ARM_CM4F 的官方移植层，不受影响。
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------
 * Type definitions.
 *-----------------------------------------------------------*/
#define portCHAR                char
#define portFLOAT               float
#define portDOUBLE              double
#define portLONG                long
#define portSHORT               short
#define portSTACK_TYPE          uint32_t
#define portBASE_TYPE           long

typedef portSTACK_TYPE StackType_t;
typedef portBASE_TYPE BaseType_t;
typedef unsigned portBASE_TYPE UBaseType_t;

#if (configUSE_16_BIT_TICKS == 1)
    typedef uint16_t TickType_t;
    #define portMAX_DELAY       ( TickType_t ) 0xffff
#else
    typedef uint32_t TickType_t;
    #define portMAX_DELAY       ( TickType_t ) 0xffffffffUL
    #define portTICK_TYPE_IS_ATOMIC 1
#endif

/*-----------------------------------------------------------
 * Architecture specifics.
 *-----------------------------------------------------------*/
#define portSTACK_GROWTH        ( -1 )
#define portTICK_PERIOD_MS      ( ( TickType_t ) 1000 / configTICK_RATE_HZ )
#define portBYTE_ALIGNMENT      8

/*-----------------------------------------------------------
 * Scheduler utilities.
 *-----------------------------------------------------------*/
extern void vPortYield( void ) __attribute__( ( naked ) );
extern void vPortYieldFromISR( BaseType_t x ) __attribute__( ( naked ) );

#define portYIELD()                         vPortYield()
#define portNVIC_INT_CTRL_REG               ( *( ( volatile uint32_t * ) 0xe000ed04 ) )
#define portNVIC_PENDSVSET_BIT              ( 1UL << 28UL )
#define portEND_SWITCHING_ISR( xSwitchRequired ) \
    if ( ( xSwitchRequired ) != pdFALSE ) { portYIELD_FROM_ISR( pdTRUE ); }
#define portYIELD_FROM_ISR( x )             vPortYieldFromISR( x )

/*-----------------------------------------------------------
 * Critical section management.
 *-----------------------------------------------------------*/
extern void vPortEnterCritical( void );
extern void vPortExitCritical( void );
extern void vPortSetBASEPRI( uint32_t ulNewMaskValue );
extern void vPortRaiseBASEPRI( void );

#define portDISABLE_INTERRUPTS()            vPortRaiseBASEPRI()
#define portENABLE_INTERRUPTS()             vPortSetBASEPRI( 0 )
#define portENTER_CRITICAL()                vPortEnterCritical()
#define portEXIT_CRITICAL()                 vPortExitCritical()

#define portSET_INTERRUPT_MASK()            __get_PRIMASK()
#define portCLEAR_INTERRUPT_MASK( uxSavedStatusRegister ) __set_PRIMASK( uxSavedStatusRegister )
#define portSET_INTERRUPT_MASK_FROM_ISR()   __get_PRIMASK()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( uxSavedStatusRegister ) \
    __set_PRIMASK( uxSavedStatusRegister )
#define portDISABLE_ALL_INTERRUPTS()        vPortRaiseBASEPRI()
#define portENABLE_ALL_INTERRUPTS()         vPortSetBASEPRI( 0 )

#define portASSERT_IF_INTERRUPT_PRIORITY_INVALID() \
    configASSERT( ( configMAX_SYSCALL_INTERRUPT_PRIORITY ) != 0 )

/*-----------------------------------------------------------
 * Misc.
 *-----------------------------------------------------------*/
#define portINLINE      __inline
#define portFORCE_INLINE inline __attribute__(( always_inline ))
#define portNOP()       __asm volatile ( " nop " )

#define portTASK_FUNCTION_PROTO( vFunction, pvParameters ) void vFunction( void *pvParameters )
#define portTASK_FUNCTION( vFunction, pvParameters ) void vFunction( void *pvParameters )

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
