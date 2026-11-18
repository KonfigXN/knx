.code64
.section .text.entry
.global knx_entry
.type knx_entry,@function
.extern _bss_start
.extern _bss_end
.extern kernel_main
knx_entry:
    cli
    movq %rdi, %r12
    leaq _bss_start(%rip), %rdi
    leaq _bss_end(%rip), %rcx
    subq %rdi, %rcx
    xorl %eax, %eax
    rep stosb
    movq %r12, %rdi
    andq $-16, %rsp
    call kernel_main
1:  hlt
    jmp 1b
.size knx_entry, .-knx_entry

.global knx_load_gdt
.type knx_load_gdt,@function
knx_load_gdt:
    lgdt (%rdi)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    ret
.size knx_load_gdt, .-knx_load_gdt

.global knx_load_tr
.type knx_load_tr,@function
knx_load_tr:
    movw $0x28, %ax
    ltr %ax
    ret
.size knx_load_tr, .-knx_load_tr
