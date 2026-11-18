OUTPUT_FORMAT(binary)
ENTRY(stage2_entry)
SECTIONS {
    . = 0x8000;
    .text : { *(.text) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.data*) *(.bss*) *(COMMON) }
}
