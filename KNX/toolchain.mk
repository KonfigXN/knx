# AArch64-only toolchain configuration.
#
# Windows builds use the LLVM tools natively; POSIX builds use the system
# cross compiler.  Both produce little-endian AArch64 ELF objects.
ifeq ($(OS),Windows_NT)

CC      = clang --target=aarch64-elf
CXX     = clang++ --target=aarch64-elf
AS      = clang --target=aarch64-elf -c
LD      = ld.lld -m aarch64elf
OBJCOPY = llvm-objcopy
RUSTC   = rustc

RUST_TARGET = aarch64-unknown-none
RUST_SYSROOT_FLAG =
ifneq ($(strip $(KNX_RUST_SYSROOT)),)
RUST_SYSROOT_FLAG = --sysroot "$(KNX_RUST_SYSROOT)"
endif

PLATFORM_STABLE_CFLAGS = -fno-unroll-loops -fno-vectorize -fno-slp-vectorize

else

CC      = aarch64-linux-gnu-gcc
CXX     = aarch64-linux-gnu-g++
AS      = aarch64-linux-gnu-as
LD      = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy
RUSTC   = rustc

RUST_TARGET = aarch64-unknown-none
RUST_SYSROOT_FLAG =
ifneq ($(strip $(KNX_RUST_SYSROOT)),)
RUST_SYSROOT_FLAG = --sysroot "$(KNX_RUST_SYSROOT)"
endif

PLATFORM_STABLE_CFLAGS =

endif
