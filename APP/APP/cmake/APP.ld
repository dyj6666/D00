/* ================================================================
 * D00 APP 链接脚本（GCC / CMake 构建）
 * 布局与 MDK-ARM/APP/APP.sct 严格一致：
 *   FLASH   0x08010000  LENGTH 0x00050000 (320KB, RUN 区)
 *   RAM     0x20000000  LENGTH 0x00020000 (128KB 主 SRAM)
 *   CCMRAM  0x10000000  LENGTH 0x00010000 (64KB, .ccmram UNINIT)
 * 注意：0x0805FFF8 magic / 0x0805FFFC version 由 OTA 流程写入，
 *       链接器不得占用（LENGTH 已限制在 0x50000 内）。
 * ================================================================ */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH  (rx)   : ORIGIN = 0x08010000, LENGTH = 0x00050000
  RAM    (xrw)  : ORIGIN = 0x20000000, LENGTH = 0x00020000
  CCMRAM (xrw)  : ORIGIN = 0x10000000, LENGTH = 0x00010000
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } > FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    end = .;                /* newlib _sbrk 堆起点 */
  } > RAM

  /* CCM RAM（CPU 独占）：FreeRTOS 堆 / 事件池 / LA 缓冲，启动不清零 */
  .ccmram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ccmram)
    *(.ccmram*)
    . = ALIGN(4);
  } > CCMRAM


  /DISCARD/ :
  {
    libc.a (*)
    libm.a (*)
    libgcc.a (*)
  }
}
