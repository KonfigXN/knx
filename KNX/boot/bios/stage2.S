.code16
.section .text
.global stage2_entry
stage2_entry:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    call serial_init
    call e820_collect
    movw $boot_message, %si
    call serial_puts
    call kernel_read
    testb %al, %al
    jnz disk_error

    cli
    lgdt gdt_desc
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $protected

disk_error:
    movw $0x3f8, %dx
    movb $'E', %al
    outb %al, %dx
    cli
1:  hlt
    jmp 1b

serial_init:
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $0x03, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x03, %al
    outb %al, %dx
    ret

serial_puts:
    lodsb
    testb %al, %al
    jz 1f
    movb %al, %bl
    movw $0x3fd, %dx
2:  inb %dx, %al
    testb $0x20, %al
    jz 2b
    movw $0x3f8, %dx
    movb %bl, %al
    outb %al, %dx
    jmp serial_puts
1:  ret

e820_collect:
    movl $0, 0x5000
    movl $0x42584e4b, 0x5000
    movl $0x31544f4f, 0x5004
    movl $4, 0x5008
    movl $1, 0x500c
    movl $0x5100, 0x5010
    movl $0, 0x5014
    movl $0, 0x5018
    movl $0, 0x501c
    movl $0, 0x5020
    movl $0, 0x5024
    movl $0, 0x5028
    movl $0, 0x502c
    movl $0, 0x5030
    movl $0, 0x5034
    movl $0, 0x5038
    movl $0, 0x503c
    movl $0, 0x5040
    movl $0, 0x5044
    movl $0, 0x5048
    movl $0, 0x504c

    movl $0, 0x5070
    movl $0, 0x5074
    movl $0, 0x5078
    movl $0, 0x507c

    movl $0, 0x5080
    movl $0, 0x5084
    movl $0, 0x5088
    movl $0, 0x508c
    movl $0, 0x5090
    movl $0, 0x5094
    movl $0, %ebx
    movw $0x5100, %di
    movw $64, %bp
2:
    movl $0xe820, %eax
    movl $0x534d4150, %edx
    movl $24, %ecx
    movl $1, 20(%di)
    int $0x15
    jc 3f
    cmpl $0x534d4150, %eax
    jne 3f
    addw $24, %di
    incl 0x5018
    decw %bp
    jz 3f
    testl %ebx, %ebx
    jnz 2b
3:  ret

kernel_read:
    movw $512, %bp
    movw $0, dap+4
    movw $0x1000, dap+6
    movl $9, dap+8
4:
    movw $dap, %si
    movb $0x42, %ah
    movb 0x4ff0, %dl
    pushw %bp
    int $0x13
    popw %bp
    jnc 6f

    movw dap+6, %ax
    movw %ax, %es
    movw dap+4, %bx
    movw dap+8, %ax
    xorw %dx, %dx
    movw $18, %cx
    divw %cx
    movb %dl, %cl
    incb %cl
    movb %al, %dh
    andb $1, %dh
    shrw $1, %ax
    movb %al, %ch
    movw $0x0201, %ax
    movb 0x4ff0, %dl
    int $0x13
    jc 5f
6:
    addw $0x200, dap+4
    jnc 7f
    addw $0x1000, dap+6
7:
    addl $1, dap+8
    decw %bp
    jnz 4b
    xorb %al, %al
    ret
5:  movb $1, %al
    ret

.code32
protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movl $0x10000, %esi
    movl $0x100000, %edi
    movl $65536, %ecx
    rep movsl

    movl $0x50000, %edi
    xorl %eax, %eax
    movl $4096, %ecx
    rep stosl
    movl $0x51003, 0x50000
    movl $0, 0x50004
    movl $0x52003, 0x50008
    movl $0, 0x5000c
    movl $0x53003, 0x50010
    movl $0, 0x50014
    movl $0x54003, 0x50018
    movl $0, 0x5001c

    movl $0x51000, %edi
    xorl %ebx, %ebx
    movl $4, %edx
6:  movl $512, %ecx
7:  movl %ebx, %eax
    orl $0x83, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %ebx
    addl $8, %edi
    loop 7b
    decl %edx
    jnz 6b
    movl $0x50000, %eax
    movl %eax, %cr3
    movl %cr4, %eax
    orl $0x30, %eax
    movl %eax, %cr4
    movl $0xc0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    ljmpl $0x18, $long_mode

.code64
long_mode:
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq $0x400008, %rsp
    movq $0x5000, %rdi
    movl $0x100000, %eax
    jmp *%rax

.code16
.align 8
dap:
    .byte 0x10, 0
    .word 1
    .word 0
    .word 0x1000
    .quad 1
boot_message:
    .asciz "KNX Boot Manager (BIOS): loading kernel\r\n"
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:
gdt_desc:
    .word gdt_end-gdt-1
    .long gdt
