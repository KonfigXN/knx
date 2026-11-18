#pragma once
#include <stdint.h>
#include <stddef.h>
#include "knx_vfs.h"
#include "knx_syscall.h"
#define KNX_BOOT_MAGIC 0x31544f4f42584e4bULL
#define KNX_BOOT_VERSION 4u

#define KNX_BOOT_NO_X11   0x1u
#define KNX_BOOT_CONSOLE  0x2u
#define KNX_BOOT_VERBOSE  0x4u

#define KNX_FLIGHT_PHYS 0x00070000ULL
#define KNX_FLIGHT_MAGIC 0x31474f4c4b4e584bULL
#define KNX_FLIGHT_DATA 0x20000u
struct knx_flight_header {
    uint64_t magic;
    uint32_t len;
    uint32_t wrapped;
};
#ifndef KNX_KERNEL_VERSION
#define KNX_KERNEL_VERSION "KNX-1.0-beta"
#endif
#define KNX_MEMORY_USABLE 1u
#define KNX_MEMORY_RESERVED 2u
#define KNX_MEMORY_ACPI 3u
#define KNX_BOOT_BIOS 1u
#define KNX_BOOT_UEFI 2u

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} knx_memory_region;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t boot_method;
    knx_memory_region *memory_map;
    uint64_t memory_region_count;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t pixels_per_scanline;
    uint32_t pixel_format;
    uint32_t pixel_masks[4];
    uint64_t total_memory;
    uint64_t available_memory;
    uint64_t boot_tsc;
    uint64_t acpi_rsdp;

    uint64_t initrd_base;
    uint64_t initrd_size;
    uint64_t boot_flags;

    uint64_t log_file;
    uint64_t log_write;
} knx_boot_info;

#ifdef __cplusplus
extern "C" {
#endif
void knx_puts(const char *text);
void knx_putc(char c);
void knx_flight_init(void);
void knx_flight_putc(char c);
uint32_t knx_flight_read(uint8_t *dst, uint32_t max);
uint64_t knx_boot_flags(void);
void knx_console_early_init(void);
void knx_log(const char *category, const char *message);
char knx_serial_read_char(void);

char knx_serial_read_raw(void);

int knx_console_owned(void);
void knx_fb_set_info(const knx_boot_info *info);
void knx_print_u64(uint64_t value);
void knx_print_i64(int64_t value);
void knx_set_boot_info(knx_boot_info *info);
void knx_console_init(knx_boot_info *info);

void knx_console_graphics_owner(void);
int knx_graphics_init(const knx_boot_info *info);
void knx_early_fb_stamp(const knx_boot_info *info);
void knx_gfx_putpixel(uint32_t x, uint32_t y, uint32_t color);
void knx_gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void knx_gfx_hline(uint32_t x, uint32_t y, uint32_t length, uint32_t color);
void knx_gfx_vline(uint32_t x, uint32_t y, uint32_t length, uint32_t color);
void knx_gfx_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void knx_gfx_copy(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
uint32_t knx_gfx_encode_color(uint32_t color);
void knx_gfx_blit(uint32_t x, uint32_t y, const uint32_t *src,
                  uint32_t w, uint32_t h, uint32_t src_stride);
void knx_shell_run(knx_boot_info *info);
void knx_cpu_init(void);
void knx_cpu_info(void);
void knx_cpu_topology(void);
void knx_gdt_init(void);
void knx_gdt_init_cpu(unsigned cpu);
void knx_set_kernel_stack(uint64_t stack_top);

uint64_t knx_kernel_stack_top(void);
void knx_vm_init(void);
void *knx_vm_clone_user(void);
void knx_vm_destroy_user(void *root);
void knx_vm_switch(void *root);
int64_t knx_vm_map_anonymous(uint64_t size, uint64_t permissions);
void knx_net_tick(void);

int knx_vm_map_mmio(uint64_t physical, uint64_t size);

int knx_vm_framebuffer_init(uint64_t physical, uint64_t size);

int64_t knx_vm_map_framebuffer(void);

int knx_vm_shared_init(void);

int64_t knx_vm_map_shared(void *space, uint64_t flags);
int knx_vm_unmap(uint64_t base, uint64_t size);

int knx_vm_handle_fault(uint64_t address, uint64_t error_code);

int64_t knx_vm_map_user_page(uint64_t address, uint64_t permissions);

void *knx_vm_load_elf(const void *image, uint64_t size, uint64_t *end_out, uint64_t *entry_out,
                     uint64_t stack_va, uint64_t stack_frame);
void knx_idt_init(void);
void knx_interrupts_init(void);
void knx_smp_init(knx_boot_info *info);
uint64_t knx_smp_cpu_count(void);
uint64_t knx_smp_cpu_index(void);
int knx_smp_is_bsp(void);
void knx_smp_ap_timer_init(void);
void knx_smp_ap_timer_eoi(void);
void knx_task_cpu_online(unsigned cpu);
void knx_task_selftest_wait(void);
int knx_task_is_user_context(void);
void knx_task_kill_current(void);
void knx_task_debug_current(void);
void knx_smp_info(void);
void knx_memory_init(knx_boot_info *info);
void knx_acpi_init(knx_boot_info *info);
int knx_acpi_poweroff(void);
int knx_acpi_reboot(void);
void knx_acpi_info(void);
void knx_devices_init(void);
void knx_devices_info(void);
void knx_usb_info(void);
void knx_net_init(void);
void knx_net_info(void);
int knx_net_parse_ip(const char *text, uint32_t *out);
int knx_net_socket(int domain, int type, int protocol);
int knx_net_bind(int fd, uint32_t addr, uint16_t port);
int knx_net_sendto(int fd, const void *buf, uint64_t len, uint64_t flags, uint32_t addr, uint16_t port);
int knx_net_recvfrom(int fd, void *buf, uint64_t len, uint64_t flags, uint32_t *addr, uint16_t *port);
int knx_net_close(int fd);
void knx_net_ping(uint32_t target);
void knx_net_ifconfig(void);
void knx_net_netstat(void);

uint8_t knx_io_in8(uint16_t port);
uint16_t knx_io_in16(uint16_t port);
uint32_t knx_io_in32(uint16_t port);
void knx_io_out8(uint16_t port, uint8_t value);
void knx_io_out16(uint16_t port, uint16_t value);
void knx_io_out32(uint16_t port, uint32_t value);
typedef void (*knx_irq_handler_fn)(unsigned vector, uint64_t *frame);

int knx_irq_register(unsigned vector, knx_irq_handler_fn handler);
void knx_irq_dispatch(unsigned vector, uint64_t *frame);
uint16_t knx_irq_enabled_mask(void);

typedef int (*knx_service_start_fn)(knx_boot_info *info);

#define KNX_SERVICE_CRITICAL 0x01u
enum knx_service_state {
    KNX_SERVICE_REGISTERED = 0,
    KNX_SERVICE_STARTING,
    KNX_SERVICE_RUNNING,
    KNX_SERVICE_FAILED,
    KNX_SERVICE_BLOCKED,
};
int knx_service_register(const char *name, uint32_t dependencies,
                         knx_service_start_fn start);
int knx_service_register_ex(const char *name, uint32_t dependencies, uint8_t flags,
                            knx_service_start_fn start);

int knx_services_init(knx_boot_info *info);
enum knx_service_state knx_service_state(unsigned id);
int knx_service_is_running(const char *name);
unsigned knx_service_count(void);
unsigned knx_service_failures(void);
void knx_vfs_init(void);
void knx_services_info(void);
void knx_kfs_init(void);
void knx_ufs_init(void);
int knx_ufs_ready(void);

void knx_ufs_memdisk(const void *base, uint64_t size);
int knx_kfs_ready(void);
int knx_root_mount_test(void);
int knx_kext_load_file(const char *path);
int knx_kext_unload(const char *name);
int knx_kext_selftest(void);
void knx_kfs_list(void);
int knx_kfs_cat(const char *name);
int64_t knx_kfs_read(const char *name, void *buffer, uint64_t capacity);

typedef struct {
    uint8_t bus, device, function, class_code, subclass, prog_if, header_type;
    uint16_t vendor_id, device_id;
    uint32_t bar[6];
    uint8_t interrupt_line, interrupt_pin;
} knx_pci_device;
typedef int (*knx_driver_probe_fn)(const knx_pci_device *device);

int knx_driver_register(const char *name, uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                        knx_driver_probe_fn probe);
uint32_t knx_pci_read32(const knx_pci_device *device, unsigned offset);
void knx_pci_write32(const knx_pci_device *device, unsigned offset, uint32_t value);

#define KNX_PCI_ENABLE_IO 0x01u
#define KNX_PCI_ENABLE_MEMORY 0x02u
#define KNX_PCI_ENABLE_BUS_MASTER 0x04u
int knx_pci_enable_device(const knx_pci_device *device, uint16_t flags);

uint64_t knx_pci_mmio_bar(const knx_pci_device *device, unsigned index);
int knx_xhci_probe(const knx_pci_device *device);
int knx_i2c_lpss_probe(const knx_pci_device *device);
void knx_i2c_tick(void);
void knx_warm_reboot(void);

int knx_uhci_probe(const knx_pci_device *device);
int knx_ohci_probe(const knx_pci_device *device);
int knx_ehci_probe(const knx_pci_device *device);
char knx_legacy_usbkbd_read(void);
void knx_legacy_usb_tick(void);
int knx_rtl8139_probe(const knx_pci_device *device);
int knx_e1000_probe(const knx_pci_device *device);
int knx_es1370_probe(const knx_pci_device *device);
int knx_hda_probe(const knx_pci_device *device);
void knx_audio_tick(void);
void knx_pcspk_init(void);
void knx_pcspk_beep(uint32_t hz, uint32_t ms);
void knx_pcspk_tick(void);
void knx_usb_tick(void);
char knx_usbkbd_read(void);
int knx_pcm_write(const uint8_t *buf, uint64_t len);
void *knx_alloc_page(void);
void *knx_alloc_contiguous_pages(size_t count);
void knx_free_page(void *ptr);
void *knx_heap_alloc(size_t size);
void knx_heap_free(void *ptr);
int knx_heap_selftest(void);
uint64_t knx_free_memory(void);
uint64_t knx_total_memory(void);
char knx_keyboard_read(void);

void knx_ps2_poll(void);

int knx_key_event_read(uint8_t *code, uint8_t *pressed, uint16_t *mods);

void knx_key_event_emit(uint8_t code, uint8_t pressed);

void knx_mouse_event_emit(int32_t dx, int32_t dy, uint8_t buttons);
void knx_key_mods_set(uint16_t mods);

int knx_mouse_event_read(struct knx_mouse_event *event);

void knx_fb_get_size(uint32_t *width, uint32_t *height);
uint64_t knx_ticks(void);
void knx_start_user_tasks(void);

int knx_kernel_exec(const char *path, const char *const argv[], const char *const envp[]);
uint64_t *knx_interrupt_dispatch(uint64_t vector, uint64_t error, uint64_t *frame);
int knx_syscall(uint64_t *frame);
void knx_ap_main(uint64_t apic_id);
uint64_t knx_task_count(void);
uint64_t knx_task_syscalls(void);

int knx_task_cap_slot(void);
char *knx_capture_buf(int slot);
unsigned knx_capture_len(int slot);
void knx_capture_putc(int slot, char c);
uint64_t knx_task_syscall_audit(unsigned number);
uint64_t knx_process_count(void);
extern char __kernel_end;
void knx_clear(void);
int knx_login_init(void);
int knx_login_attempt(const char *username);
int knx_login_is_authenticated(void);
const char *knx_login_user(void);
int knx_cpp_command(const char *command);
uint64_t knx_rust_value(void);
uint64_t knx_rust_add(uint64_t a, uint64_t b);
#ifdef __cplusplus
}
#endif
