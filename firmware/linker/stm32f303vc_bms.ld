/* stm32f303vc_bms.ld — Linker script for BMS application on STM32F303VC.
 *
 * Flash map:
 *   0x08000000 Bootloader  32KB
 *   0x08008000 Application 186KB  ← this image (last 2K page = BL metadata)
 *   0x08037000 Config A    8KB
 *   0x08039000 Config B    8KB
 *
 * RAM: 40KB SRAM at 0x20000000.
 *
 * PROVISIONAL: verify page alignment and exact sizes after hardware confirmation.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 186K  /* last page reserved for bootloader metadata */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 40K
}

/* Entry point */
ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table and code */
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

    /* ARM exception index table */
    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* Initialised data load address in flash */
    _sidata = LOADADDR(.data);

    /* Initialised data in RAM */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* Zero-initialised data */
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

    /* Stack at top of RAM */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_end = .;
        . += 4096;    /* 4KB stack — adjust if needed */
        . = ALIGN(8);
        _stack_start = .;
    } > RAM

}

/* Export symbols used by startup code */
_estack = _stack_start;
