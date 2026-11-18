# KNX Kernel

**KNX** is an original x86_64 kernel developed from scratch using freestanding C, C++, Rust, and assembly. It is an independent kernel implementation and contains no Linux kernel code.

This repository contains the **kernel source, BIOS and UEFI bootloaders, and a kernel-only bootable ISO**. Userspace, BusyBox, desktop environments, and the X11 stack are maintained as separate projects and are intentionally excluded from this build.

KNX is designed around a lightweight, modular architecture, with a focus on reliability, hardware support, and long-term development.

**License:** GPL-3.0-only. The complete license text is available in `COPYING` at the repository root and is also included at the root of the generated ISO.

## Project Structure

```text
boot/
  bios/             16-bit boot stages and El Torito kernel loader
  uefi/             UEFI application and long-mode handoff

kernel/             Core kernel implementation
                    CPU, memory management, interrupts, SMP,
                    VFS, filesystems, drivers, networking,
                    console, shell, task management, Rust and C++

include/            Kernel headers and public interfaces
                    System call and boot information ABI

docs/               Architecture, boot process, storage,
                    UFS, process management and licensing

Makefile            Kernel, bootloader and ISO build system
toolchain.mk        Shared GCC and LLVM toolchain configuration
COPYING             GPL-3.0-only license
```

## Building KNX

### Requirements

The build environment requires:

- Clang and LLD
- GCC and GNU binutils (POSIX environments)
- Rust with the `x86_64-unknown-none` target
- GNU Make
- `xorriso`, mtools and dosfstools (for ISO generation)

### Build Targets

```sh
make                 # Build using the default stable profile
make stable          # Build the stable profile
make debug           # Build the debug profile
make builds          # Build both stable and debug profiles

make kernel          # Build kernel.elf only
make kernel-bin      # Build ELF and raw kernel.bin
make iso             # Generate the kernel-only bootable ISO

make clean           # Remove generated build artifacts
```

The default build produces:

```text
build/knx.iso
```

Profile-specific builds are placed in their respective directories:

```text
build/
  stable/
    knx.iso
  debug/
    knx.iso
```

### Windows

Windows builds are supported through the provided PowerShell build script.

From the repository root, run:

```powershell
../build-windows.ps1
```

The script configures the LLVM and MSYS2 toolchain before invoking the Makefile.

## Boot Architecture

KNX supports both legacy BIOS and UEFI boot environments.

The BIOS and UEFI loaders initialize the required execution environment, prepare the boot information structure, and transfer control to the kernel in x86_64 long mode.

Both loaders pass a `knx_boot_info` structure to the kernel and enter its execution point at physical address `0x100000`.

### Root Filesystem Handoff

An external OS bootloader can provide an in-memory UFS1 root filesystem image through the following boot information fields:

- `initrd_base` — Physical base address of the root image.
- `initrd_size` — Size of the supplied image.

The kernel can also boot without an initial root filesystem or attached storage device. In this configuration, it reaches its built-in shell while skipping checks that depend on filesystem availability.

The generated ISO contains:

- A BIOS El Torito boot entry.
- A UEFI boot entry.
- The kernel and required boot components.

It does not contain a userspace image or a separate data disk.

**Boot testing:** This repository does not include QEMU launch scripts or an automated boot-testing environment. The generated ISO can be launched using external virtualization or hardware-testing tools.

## System Interfaces and ABI

KNX exposes project-specific interfaces for communication between the kernel and its callers.

The primary public interfaces are:

| Header | Purpose |
|---|---|
| `include/knx_syscall.h` | System call numbers and wrappers |
| `include/knx.h` | Boot information structure and related definitions |

The system call interface and boot ABI are currently under development and **are not considered stable**. Their definitions may change as the kernel evolves.

## Documentation

Technical documentation is maintained separately under `docs/`.

Start with:

```text
docs/README.md
```

It provides an index of the available documentation, covering kernel architecture, boot procedures, storage, UFS, process management, and other implementation details.

The changelog at the repository root predates the separation of the kernel and userspace projects. As a result, it also contains historical entries covering earlier userspace and X11 development.

---

**KNX Kernel — An independent kernel implementation, built from the ground up.**