    .section .ap_trampoline,"ax"
    .code16
    .global knx_ap_trampoline_start
    .global knx_ap_trampoline_end

knx_ap_trampoline_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    cld
    lgdtw (0x8000 + (ap_gdt_desc - knx_ap_trampoline_start))

    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x18, $(0x8000 + (ap_protected - knx_ap_trampoline_start))

    .code32
ap_protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl (0x9000), %eax
    movl %eax, %cr3
    movl %cr4, %eax
    orl $0x630, %eax
    movl %eax, %cr4
    movl $0xc0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(0x8000 + (ap_long_mode - knx_ap_trampoline_start))

    .code64
ap_long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movabsq $0x9018, %rax
    cmpq $0, (%rax)
    je .Lxapic_id
    movl $0x1b, %ecx
    rdmsr
    orl $0xc00, %eax
    wrmsr
    movl $0x802, %ecx
    rdmsr
    movl %eax, %ecx
    jmp .Lhave_id
.Lxapic_id:
    movabsq $0xfee00020, %rax
    movl (%rax), %ecx
    shrl $24, %ecx
.Lhave_id:

    movabsq $0x9008, %rax
    movq (%rax), %rsi
    movabsq $0x9020, %rax
    movq (%rax), %rdx
    xorq %r8, %r8
.Lscan:
    cmpq %rdx, %r8
    jae .Lbad_id
    movq %r8, %r9
    shlq $4, %r9
    movl 8(%rsi,%r9), %eax
    cmpl %ecx, %eax
    je .Lfound
    incq %r8
    jmp .Lscan
.Lbad_id:
    cli
    hlt
    jmp .Lbad_id
.Lfound:
    movq (%rsi,%r9), %rsp
    andq $-16, %rsp
    pushq $0
    movq %rcx, %rdi
    movabsq $0x9010, %rax
    movq (%rax), %rax
    jmp *%rax

    .align 16
ap_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00cf9a000000ffff
ap_gdt_end:
ap_gdt_desc:
    .word ap_gdt_end - ap_gdt - 1
    .long 0x8000 + (ap_gdt - knx_ap_trampoline_start)
knx_ap_trampoline_end:
    .section .note.GNU-stack,"",@progbits
