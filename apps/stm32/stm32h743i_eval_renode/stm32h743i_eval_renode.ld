/* STM32H743xI memory layout for Renode: Flash, DTCM, AXI SRAM, and D2 SRAM. */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    DTCMRAM(rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    RAM_D1 (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
    RAM_D2 (rwx) : ORIGIN = 0x30000000, LENGTH = 288K
}

SECTIONS
{
    .isr_vector : ALIGN(4)
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __etext = .;
    } > FLASH

    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM_D1 AT > FLASH
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } > RAM_D1

    . = ALIGN(8);
    __heap_start__ = .;

    .stack ORIGIN(RAM_D1) + LENGTH(RAM_D1) - 0x8000 (NOLOAD) : ALIGN(8)
    {
        __stack_limit__ = .;
        . = . + 0x8000;
        __stack_top__ = .;
    } > RAM_D1
    __heap_end__ = __stack_limit__;

    .eth_dma (NOLOAD) : ALIGN(32)
    {
        *(.RxDescripSection)
        *(.TxDescripSection)
        *(.RxPoolSection)
        *(.TxPoolSection)
        . = ALIGN(32);
    } > RAM_D2
}
