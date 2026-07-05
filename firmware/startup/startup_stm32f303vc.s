/* startup_stm32f303vc.s — Minimal Cortex-M4 startup for STM32F303VC.
 * Copies .data from flash to RAM, zeros .bss, calls main().
 * Does not provide a full IRQ vector table — extend as peripherals are added.
 */

    .syntax unified
    .cpu cortex-m4
    .fpu softvfp
    .thumb

/* External symbols from linker script */
    .global _estack
    .global _sidata
    .global _sdata
    .global _edata
    .global _sbss
    .global _ebss

/* ── Vector table ───────────────────────────────────────────────────────── */
    .section .isr_vector,"a",%progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack               /* Initial MSP */
    .word Reset_Handler         /* Reset                     */
    .word Default_Handler       /* NMI                       */
    .word HardFault_Handler     /* HardFault                 */
    .word Default_Handler       /* MemManage                 */
    .word Default_Handler       /* BusFault                  */
    .word Default_Handler       /* UsageFault                */
    .word 0                     /* Reserved                  */
    .word 0                     /* Reserved                  */
    .word 0                     /* Reserved                  */
    .word 0                     /* Reserved                  */
    .word Default_Handler       /* SVC                       */
    .word Default_Handler       /* DebugMon                  */
    .word 0                     /* Reserved                  */
    .word Default_Handler       /* PendSV                    */
    .word SysTick_Handler       /* SysTick                   */
    /* IRQs 0-37 — default handler */
    .rept 38
    .word Default_Handler
    .endr
    .word USART2_IRQHandler     /* IRQ 38: USART2 global     */
    /* IRQs 39-81 — default handler */
    .rept 43
    .word Default_Handler
    .endr

/* ── Reset handler ──────────────────────────────────────────────────────── */
    .section .text.Reset_Handler
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* Set MSP (redundant if loaded by hardware, but safe) */
    ldr r0, =_estack
    mov sp, r0

    /* Copy .data section from flash to RAM */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
    b   copy_check
copy_loop:
    ldr r3, [r2], #4
    str r3, [r0], #4
copy_check:
    cmp r0, r1
    blt copy_loop

    /* Zero .bss section */
    ldr r0, =_sbss
    ldr r1, =_ebss
    mov r2, #0
    b   zero_check
zero_loop:
    str r2, [r0], #4
zero_check:
    cmp r0, r1
    blt zero_loop

    /* Call main */
    bl main

    /* If main returns, spin */
hang:
    b hang

/* ── Fault handlers ─────────────────────────────────────────────────────── */
    .section .text.HardFault_Handler
    .weak HardFault_Handler
    .type HardFault_Handler, %function
HardFault_Handler:
    b HardFault_Handler         /* Spin; IWDG will reset */

/* ── Default handler ────────────────────────────────────────────────────── */
    .section .text.Default_Handler
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b Default_Handler

/* ── Weak aliases for handlers defined elsewhere (e.g. board_clock.c) ──── */
    .weak SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler

    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler, Default_Handler

    .size Reset_Handler, .-Reset_Handler
