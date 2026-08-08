/* APP 固件链接脚本（GCC）
 *
 * 分区布局（与 BOOT/boot_config.h、APP.sct 严格一致）：
 *   BOOT     0x08000000 ~ 0x0800FFFF  (64 KB)
 *   APP/RUN  0x08010000 ~ 0x0805FFFF  (320 KB，末尾 8 字节为魔数+版本)
 *   BACKUP   0x08060000 ~ 0x0809FFFF  (256 KB)
 *   DOWNLOAD 0x080A0000 ~ 0x080DFFFF  (256 KB)
 *   PARAM    0x080E0000 ~ 0x080FFFFF  (128 KB)
 * 主 SRAM 128 KB：0x20000000 ~ 0x2001FFFF。
 */
ENTRY(Reset_Handler)

/* 堆栈大小（FreeRTOS 任务栈由 heap_4 分配，此处仅保证 C 启动/ISR 需要） */
_Min_Heap_Size = 0x400;
_Min_Stack_Size = 0x800;

_estack = ORIGIN(RAM) + LENGTH(RAM);

MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08010000, LENGTH = 0x50000   /* 320 KB */
  RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 0x20000   /* 128 KB */
  MEMORY_B1 (rx)  : ORIGIN = 0x60000000, LENGTH = 0K
  CCMRAM (rw)     : ORIGIN = 0x10000000, LENGTH = 0K
}

SECTIONS
{
  /* 中断向量表：必须位于 APP 起始地址 */
  .isr_vector :
  {
    . = ALIGN(8);
    KEEP(*(.isr_vector))
    . = ALIGN(8);
  } >FLASH

  /* 程序代码 */
  .text :
  {
    . = ALIGN(8);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(8);
    _etext = .;
  } >FLASH

  /* 只读数据 */
  .rodata :
  {
    . = ALIGN(8);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(8);
  } >FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  /* 初始化数据（从 Flash 拷贝到 RAM） */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(8);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(8);
    _edata = .;
  } >RAM AT> FLASH

  /* 未初始化数据 */
  . = ALIGN(8);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* 堆与栈 */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 :
  {
    *(.ARM.attributes)
  }
}
