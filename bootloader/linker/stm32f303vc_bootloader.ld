/* stm32f303vc_bootloader.ld — Linker script for STM32F303VC bootloader.
 *
 * Flash map:
 *   0x08000000 Bootloader  32KB  ← this image
 *   0x08008000 Application 188KB
 *   0x08037000 Config A    8KB
 *   0x08039000 Config B    8KB
 *
 * RAM: 40KB SRAM at 0x20000000.
 *
 * PROVISIONAL: verify page alignment and exact sizes after hardware confirmation.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 40K
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* Guarantee we never spill into the application region */
    ASSERT(. <= 0x08008000, "Bootloader exceeds 32 KB — overlaps application region");

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_end = .;
        . += 2048;    /* 2KB stack — bootloader has minimal call depth */
        . = ALIGN(8);
        _stack_start = .;
    } > RAM
}

_estack = _stack_start;
