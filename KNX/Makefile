-include toolchain.mk

BUILD ?= build
PROFILE ?= stable

COMMON_CFLAGS := -m64 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -Iinclude
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

KERNEL_C := kernel/main.c kernel/libc.c kernel/graphics.c kernel/console.c kernel/cpu/cpu.c kernel/cpu/gdt.c kernel/interrupts.c kernel/smp.c kernel/task.c kernel/memory/pmm.c kernel/memory/vm.c kernel/vfs.c kernel/fs.c kernel/fs_ufs.c kernel/kext.c kernel/login.c kernel/acpi.c kernel/device.c kernel/driver.c kernel/service.c kernel/usb/xhci.c kernel/usb/legacy.c kernel/i2c/i2c-lpss.c kernel/net.c kernel/shell/shell.c kernel/audio.c kernel/sound_hda.c kernel/pcspk.c
KERNEL_CPP := kernel/cpp.cpp
KERNEL_ASM := kernel/entry.S kernel/interrupts.S kernel/driver_io.S kernel/ap_trampoline.S
OBJ := $(patsubst kernel/%.c,$(BUILD)/c/%.o,$(KERNEL_C)) \
       $(patsubst kernel/%.cpp,$(BUILD)/cpp/%.o,$(KERNEL_CPP)) \
       $(patsubst kernel/%.S,$(BUILD)/asm/%.o,$(KERNEL_ASM)) \
       $(BUILD)/rust/rust.o
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

$(BUILD)/bios-boot.o: boot/bios/boot.S | $(BUILD)
	$(AS32) $(ASM_DEBUG_FLAGS) $< -o $@
$(BUILD)/bios-boot.bin: $(BUILD)/bios-boot.o boot/bios/boot.ld
	$(LD32) -T boot/bios/boot.ld $< -o $@
	@test $$(wc -c < $@) -eq 512
$(BUILD)/stage2.o: boot/bios/stage2.S | $(BUILD)
	$(AS32) $(ASM_DEBUG_FLAGS) $< -o $@
$(BUILD)/stage2.bin: $(BUILD)/stage2.o boot/bios/stage2.ld
	$(LD32) -T boot/bios/stage2.ld $< -o $@
	@test $$(wc -c < $@) -le 4096 || { echo 'BIOS stage 2 exceeds 8 sectors'; exit 1; }
	truncate -s 4096 $@

$(BUILD)/c/%.o: kernel/%.c include/knx.h | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@
$(BUILD)/cpp/%.o: kernel/%.cpp include/knx.h | $(BUILD)
	mkdir -p $(@D)
	$(CXX) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -fno-exceptions -fno-rtti -c $< -o $@
$(BUILD)/asm/%.o: kernel/%.S | $(BUILD)
	mkdir -p $(@D)
	$(AS64) $(ASM_DEBUG_FLAGS) $< -o $@
$(BUILD)/rust/rust.o: kernel/rust.rs | $(BUILD)
	mkdir -p $(@D)
	$(RUSTC) --crate-type lib --emit=obj --target $(RUST_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C relocation-model=static -C code-model=kernel $(RUST_SYSROOT_FLAG) $(if $(filter debug,$(PROFILE)),-C debuginfo=2 --cfg knx_debug) $< -o $@

$(BUILD)/kernel.elf: $(OBJ) kernel/link.ld Makefile
	$(LD64) $(ELF_LINK_FLAGS) -nostdlib -T kernel/link.ld $(OBJ) -o $@
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(wc -c < $@) -le 262144 || { echo 'Kernel image exceeds the 256 KiB BIOS loader limit'; exit 1; }

$(BUILD)/knx-bios.img: $(BUILD)/bios-boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin
	dd if=/dev/zero of=$@ bs=512 count=265 status=none
	dd if=$(BUILD)/bios-boot.bin of=$@ conv=notrunc status=none
	dd if=$(BUILD)/stage2.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=9 conv=notrunc status=none
$(BUILD)/bios-floppy.img: $(BUILD)/knx-bios.img
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BUILD)/knx-bios.img of=$@ conv=notrunc status=none

$(BUILD)/uefi-boot.obj: boot/uefi/boot.c include/knx.h | $(BUILD)
	clang --target=x86_64-pc-win32-coff -ffreestanding -fno-builtin -fshort-wchar -fno-stack-protector -mno-red-zone -Wall -Wextra -Werror $(PROFILE_CFLAGS) -Iinclude -MMD -MP -MF $(BUILD)/uefi-boot.d -c $< -o $@
$(BUILD)/uefi-handoff.obj: boot/uefi/handoff.S | $(BUILD)
	clang --target=x86_64-pc-win32-coff -c $< -o $@
$(BUILD)/BOOTX64.EFI: $(BUILD)/uefi-boot.obj $(BUILD)/uefi-handoff.obj
	lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib -machine:x64 -out:$@ $^

$(BUILD)/esp-iso.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin
	dd if=/dev/zero of=$@ bs=1M count=2 status=none
	mkfs.fat -F 12 $@ >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/KNX
	mcopy -i $@ $(BUILD)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(BUILD)/kernel.bin ::/EFI/KNX/KERNEL.BIN
$(BUILD)/iso-root/.stamp: $(BUILD)/bios-floppy.img $(BUILD)/esp-iso.img $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin README.md ../COPYING
	rm -rf $(BUILD)/iso-root
	mkdir -p $(BUILD)/iso-root/BIOS $(BUILD)/iso-root/EFI/BOOT $(BUILD)/iso-root/EFI/KNX
	cp $(BUILD)/bios-floppy.img $(BUILD)/iso-root/BIOS/KNX-FLOPPY.IMG
	cp $(BUILD)/esp-iso.img $(BUILD)/iso-root/EFI/ESP.IMG
	cp $(BUILD)/BOOTX64.EFI $(BUILD)/iso-root/EFI/BOOT/BOOTX64.EFI
	cp $(BUILD)/kernel.bin $(BUILD)/iso-root/EFI/KNX/KERNEL.BIN
	cp README.md $(BUILD)/iso-root/README.TXT
	cp ../COPYING $(BUILD)/iso-root/COPYING
	touch $@
$(BUILD)/knx.iso: $(BUILD)/iso-root/.stamp
	@command -v xorriso >/dev/null || { echo 'xorriso is required to create the bootable ISO'; exit 1; }
	xorriso -as mkisofs -R -J -V KNXKERNEL -iso-level 3 -o $@ \
		-c boot.catalog -b BIOS/KNX-FLOPPY.IMG \
		-eltorito-alt-boot -e EFI/ESP.IMG -no-emul-boot -isohybrid-gpt-basdat \
		$(BUILD)/iso-root

clean:
	rm -rf $(BUILD)
