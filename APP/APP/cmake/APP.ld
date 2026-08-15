/* ================================================================
 * D00 APP 链接脚本（GCC / CMake 构建）
 * 布局与 MDK-ARM/APP/APP.sct 严格一致（方案B）：
 *   FLASH   0x08010000  LENGTH 0x000D0000 (832KB, RUN 区，含原 BACKUP+DOWNLOAD)
 *   RAM     0x20000000  LENGTH 0x00030000 (192KB 主 SRAM：SRAM1+2 + SRAM3)
 *   CCMRAM  0x10000000  LENGTH 0x00010000 (64KB, .ccmram UNINIT)
 * 注意：0x080DFFF8 magic / 0x080DFFFC version 由 OTA 流程写入，
 *       链接器不得占用（LENGTH 已限制在 0xD0000 内）。
 * ================================================================ */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH  (rx)   : ORIGIN = 0x08010000, LENGTH = 0x000D0000
  RAM    (xrw)  : ORIGIN = 0x20000000, LENGTH = 0x00030000
  CCMRAM (xrw)  : ORIGIN = 0x10000000, LENGTH = 0x00010000
}

/* 主栈顶必须落在 RAM 有效范围内：
 * 0x20020000 是 128KB SRAM 的末端+1（越界），BOOT 跳转尾声会从
 * 新栈顶弹栈，越界立即触发精确总线错误（HardFault 复位循环）。
 * 留 1KB 安全余量，与 Keil 工程（__initial_sp 位于 RAM 内）行为对齐。 */
_estack = ORIGIN(RAM) + LENGTH(RAM) - 0x400;

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

  /* libc 构造/析构桩：必须 KEEP 完整收集 crti.o（入口）+ crtn.o（尾声），
   * 否则 --gc-sections 会丢弃 crtn 的 .init/.fini 尾声，_init 执行后
   * 落穿到后续数据区触发 UNDEFINSTR（实测 HardFault）。 */
  .init :
  {
    KEEP (*(SORT_NONE(.init)))
  } > FLASH

  .fini :
  {
    KEEP (*(SORT_NONE(.fini)))
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
