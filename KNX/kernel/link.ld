ENTRY(knx_entry)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS {
  . = 0x100000;
  .text : { *(.text.entry) *(.text*) *(.ap_trampoline) } :text
  . = ALIGN(4096);
  __user_text_start = .;
  .usertext : { *(.usertext*) } :text
  . = ALIGN(4096);
  __user_text_end = .;
  .rodata : { *(.rodata*) } :text
  .data : { *(.data*) } :data
  . = ALIGN(4096);
  _bss_start = .;
  .bss : { *(COMMON) *(.bss*) } :data
  . = ALIGN(4096);
  __user_stack_start = .;
  .userstack (NOLOAD) : { *(.userstack*) } :data
  . = ALIGN(4096);
  __user_stack_end = .;
  . = ALIGN(4096);
  _bss_end = .;
  __kernel_end = .;

  ASSERT(__kernel_end <= 0x400000, "kernel data/BSS exceeds the 4 MiB user window")
  /DISCARD/ : { *(.comment*) *(.eh_frame*) *(.note*) }
}
