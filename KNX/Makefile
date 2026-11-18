-include toolchain.mk

BUILD ?= build
PROFILE ?= stable

COMMON_CFLAGS := -ffreestanding -fno-pic -fno-stack-protector -fno-builtin \
                 -mgeneral-regs-only -fno-asynchronous-unwind-tables \
                 -Wall -Wextra -Werror -Iinclude
ifeq ($(PROFILE),stable)
PROFILE_CFLAGS := -O2 -DKNX_QUIET_LOGS $(PLATFORM_STABLE_CFLAGS)
ELF_LINK_FLAGS := -s --build-id=none
RUST_OPT_LEVEL := s
ASM_DEBUG_FLAGS :=
else ifeq ($(PROFILE),debug)
PROFILE_CFLAGS := -Og -g3 -DKNX_DEBUG -DKNX_PS2_DEBUG -DKNX_AUDIO_TEST_PATTERN -DKNX_KERNEL_VERSION=\"KNX-1.0-beta_debug\"
ELF_LINK_FLAGS :=
RUST_OPT_LEVEL := 0
ASM_DEBUG_FLAGS := -g
else
$(error PROFILE must be stable or debug)
endif
CFLAGS := $(COMMON_CFLAGS) $(PROFILE_CFLAGS)

KERNEL_C := kernel/main.c kernel/libc.c kernel/graphics.c kernel/console.c \
            kernel/cpu/cpu.c kernel/cpu/percpu.c kernel/interrupts.c kernel/smp.c \
            kernel/task.c kernel/memory/pmm.c kernel/memory/vm.c kernel/vfs.c \
            kernel/fs.c kernel/fs_ufs.c kernel/kext.c kernel/login.c kernel/acpi.c \
            kernel/device.c kernel/driver.c kernel/service.c kernel/usb/xhci.c \
            kernel/net.c kernel/shell/shell.c kernel/rust_stub.c kernel/arch_stubs.c
KERNEL_CPP := kernel/cpp.cpp
KERNEL_ASM := kernel/entry.S kernel/interrupts.S
OBJ := $(patsubst kernel/%.c,$(BUILD)/c/%.o,$(KERNEL_C)) \
       $(patsubst kernel/%.cpp,$(BUILD)/cpp/%.o,$(KERNEL_CPP)) \
       $(patsubst kernel/%.S,$(BUILD)/asm/%.o,$(KERNEL_ASM))
C_DEPFILES := $(patsubst kernel/%.c,$(BUILD)/c/%.d,$(KERNEL_C))
CPP_DEPFILES := $(patsubst kernel/%.cpp,$(BUILD)/cpp/%.d,$(KERNEL_CPP))
OTHER_DEPFILES := $(BUILD)/uefi-boot.d
-include $(C_DEPFILES) $(CPP_DEPFILES) $(OTHER_DEPFILES)

.PHONY: all stable debug builds kernel kernel-bin iso clean
all: iso

stable:
	$(MAKE) BUILD=build/stable PROFILE=stable all
debug:
	$(MAKE) BUILD=build/debug PROFILE=debug all
builds:
	$(MAKE) stable
	$(MAKE) debug

kernel:
	$(MAKE) BUILD=build/kernel PROFILE=stable build/kernel/kernel.elf
kernel-bin:
	$(MAKE) BUILD=build/kernel PROFILE=stable build/kernel/kernel.bin

iso: $(BUILD)/knx.iso

$(BUILD):
	mkdir -p $@

$(BUILD)/c/%.o: kernel/%.c include/knx.h | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@
$(BUILD)/cpp/%.o: kernel/%.cpp include/knx.h | $(BUILD)
	mkdir -p $(@D)
	$(CXX) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -fno-exceptions -fno-rtti -c $< -o $@
$(BUILD)/asm/%.o: kernel/%.S | $(BUILD)
	mkdir -p $(@D)
	$(AS) $(ASM_DEBUG_FLAGS) $< -o $@

$(BUILD)/kernel.elf: $(OBJ) kernel/link.ld Makefile
	$(LD) $(ELF_LINK_FLAGS) -nostdlib -T kernel/link.ld $(OBJ) -o $@
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# --- AArch64 UEFI loader ---------------------------------------------------
# AArch64 firmware looks for EFI/BOOT/BOOTAA64.EFI.

$(BUILD)/uefi-boot.o: boot/uefi/boot.c include/knx.h | $(BUILD)
	clang --target=aarch64-pc-windows-msvc -ffreestanding -fno-builtin \
		-fshort-wchar -fno-stack-protector -Wall -Wextra -Werror \
		$(PROFILE_CFLAGS) -Iinclude -MMD -MP -MF $(BUILD)/uefi-boot.d -c $< -o $@
$(BUILD)/BOOTAA64.EFI: $(BUILD)/uefi-boot.o
	lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
		-machine:arm64 -out:$@ $^

$(BUILD)/esp-iso.img: $(BUILD)/BOOTAA64.EFI $(BUILD)/kernel.bin
	dd if=/dev/zero of=$@ bs=1M count=2 status=none
	mkfs.fat -F 12 $@ >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/KNX
	mcopy -i $@ $(BUILD)/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
	mcopy -i $@ $(BUILD)/kernel.bin ::/EFI/KNX/KERNEL.BIN

$(BUILD)/iso-root/.stamp: $(BUILD)/BOOTAA64.EFI $(BUILD)/kernel.bin $(BUILD)/esp-iso.img README.md ../LICENSE
	rm -rf $(BUILD)/iso-root
	mkdir -p $(BUILD)/iso-root/EFI/BOOT $(BUILD)/iso-root/EFI/KNX
	cp $(BUILD)/BOOTAA64.EFI $(BUILD)/iso-root/EFI/BOOT/BOOTAA64.EFI
	cp $(BUILD)/kernel.bin $(BUILD)/iso-root/EFI/KNX/KERNEL.BIN
	cp $(BUILD)/esp-iso.img $(BUILD)/iso-root/EFI/ESP.IMG
	cp README.md $(BUILD)/iso-root/README.TXT
	cp ../LICENSE $(BUILD)/iso-root/COPYING
	touch $@
$(BUILD)/knx.iso: $(BUILD)/iso-root/.stamp
	@command -v xorriso >/dev/null || { echo 'xorriso is required to create the bootable ISO'; exit 1; }
	xorriso -as mkisofs -R -J -V KNXKERNEL -iso-level 3 -o $@ \
		-eltorito-alt-boot -e EFI/ESP.IMG -no-emul-boot -isohybrid-gpt-basdat \
		$(BUILD)/iso-root

clean:
	rm -rf $(BUILD)
