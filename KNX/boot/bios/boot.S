.code16
.section .text
.global _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    movb %dl, 0x4ff0
    movw $dap, %si
    movb $0x42, %ah
    int $0x13
    jnc stage2_loaded

    movb 0x4ff0, %dl
    movw $0x8000, %bx
    movw $0x0208, %ax
    movw $0x0002, %cx
    xorb %dh, %dh
    int $0x13
    jc failed
stage2_loaded:
    ljmp $0, $0x8000
failed:
    movw $0x4f45, 0xb800
    cli
1:  hlt
    jmp 1b
.align 8
dap:
    .byte 0x10, 0
    .word 8
    .word 0x8000
    .word 0
    .quad 1
.org 446

.byte 0x80, 0x00, 0x02, 0x00, 0xda, 0xfe, 0xff, 0xff
.long 1
.long 264
.org 510
.word 0xaa55
