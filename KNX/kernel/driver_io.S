.code64
.section .text
.global knx_io_in8
.type knx_io_in8,@function
knx_io_in8:
    movw %di, %dx
    xorl %eax, %eax
    inb %dx, %al
    ret
.global knx_io_in16
.type knx_io_in16,@function
knx_io_in16:
    movw %di, %dx
    xorl %eax, %eax
    inw %dx, %ax
    ret
.global knx_io_in32
.type knx_io_in32,@function
knx_io_in32:
    movw %di, %dx
    inl %dx, %eax
    ret
.global knx_io_out8
.type knx_io_out8,@function
knx_io_out8:
    movw %di, %dx
    movb %sil, %al
    outb %al, %dx
    ret
.global knx_io_out16
.type knx_io_out16,@function
knx_io_out16:
    movw %di, %dx
    movw %si, %ax
    outw %ax, %dx
    ret
.global knx_io_out32
.type knx_io_out32,@function
knx_io_out32:
    movw %di, %dx
    movl %esi, %eax
    outl %eax, %dx
    ret
.section .note.GNU-stack,"",@progbits
