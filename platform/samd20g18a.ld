/* Stock bootloader: 0; application: 0x4000; OTA staging: 0x22000.
 * Bootloader compares size against 0x1de00, but its length metadata is 16-bit.
 * Keep the initial project within that more restrictive format. */
ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x00004000, LENGTH = 0xffff
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 32K
}
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    .data : { . = ALIGN(4); _data_start = .; *(.data*) . = ALIGN(4); _data_end = .; } > RAM AT> FLASH
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    /DISCARD/ : { *(.comment*) *(.note*) }
}
ASSERT(ADDR(.vectors) == 0x4000, "application vector moved")
ASSERT(SIZEOF(.vectors) == 164, "wrong SAM D20 vector count")
ASSERT(_bss_end <= _stack_top - 4096, "leave at least 4 KiB for stack")
