ifeq ($(OS),Windows_NT)

CC      = clang --target=x86_64-elf
CXX     = clang++ --target=x86_64-elf
AS32    = clang --target=i386-elf -c
AS64    = clang --target=x86_64-elf -c
LD32    = ld.lld -m elf_i386
LD64    = ld.lld -m elf_x86_64
OBJCOPY = llvm-objcopy
RUSTC   = rustc

RUST_TARGET = x86_64-unknown-none
RUST_SYSROOT_FLAG =
ifneq ($(strip $(KNX_RUST_SYSROOT)),)
RUST_SYSROOT_FLAG = --sysroot "$(KNX_RUST_SYSROOT)"
endif

PLATFORM_STABLE_CFLAGS = -fno-unroll-loops -fno-vectorize -fno-slp-vectorize

else

CC      = gcc
CXX     = g++
AS32    = as --32
AS64    = as --64
LD32    = ld -m elf_i386
LD64    = ld -m elf_x86_64
OBJCOPY = objcopy
RUSTC   = rustc

RUST_TARGET = x86_64-unknown-linux-gnu
RUST_SYSROOT_FLAG =
PLATFORM_STABLE_CFLAGS =

endif
