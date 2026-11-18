#include <stdint.h>
#include "knx.h"
#define EFIAPI __attribute__((ms_abi))

#ifndef KNX_TSC_HZ
#define KNX_TSC_HZ 2000000000ULL
#endif
#define KNX_TSC_PER_MS (KNX_TSC_HZ / 1000ULL)

#ifndef KNX_BOOT_MENU_TIMEOUT_TSC
#define KNX_BOOT_MENU_TIMEOUT_TSC 3000000000ULL
#endif

#ifndef KNX_SERIAL_TX_TIMEOUT_TSC
#define KNX_SERIAL_TX_TIMEOUT_TSC 50000000ULL
#endif

#ifndef KNX_BOOT_TIMING
#define KNX_BOOT_TIMING 1
#endif

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef struct {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
} EFI_TABLE_HEADER;
typedef struct {
    uint32_t type, pad;
    uint64_t physical_start, virtual_start, pages, attribute;
} EFI_MEMORY_DESCRIPTOR;
typedef struct {
    uint64_t revision;
    void *open_volume;
} EFI_SIMPLE_FS;
typedef struct EFI_FILE EFI_FILE;
struct EFI_FILE {
    uint64_t revision;
    EFI_STATUS(EFIAPI *open)(EFI_FILE *, EFI_FILE **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS(EFIAPI *close)(EFI_FILE *);
    void *delete_file;
    EFI_STATUS(EFIAPI *read)(EFI_FILE *, uint64_t *, void *);
    void *rest[10];
};
typedef struct {
    uint32_t version, hres, vres, pixel_format;
    uint32_t pixel_info[4];
    uint32_t pixels_per_scanline;
} EFI_GOP_INFO;
typedef struct {
    uint32_t max_mode, mode;
    EFI_GOP_INFO *info;
    uint64_t info_size, fb_base, fb_size;
} EFI_GOP_MODE;
typedef struct {
    void *query_mode;
    void *set_mode;
    void *blt;
    EFI_GOP_MODE *mode;
} EFI_GOP;
typedef struct {
    EFI_TABLE_HEADER header;
    CHAR16 *vendor;
    uint32_t revision;
    EFI_HANDLE console_in_handle;
    void *console_in;
    EFI_HANDLE console_out_handle;
    void *console_out;
    EFI_HANDLE stderr_handle;
    void *stderr;
    void *runtime_services;
    void *boot_services;
    uint64_t table_entries;
    void *configuration_table;
} EFI_SYSTEM_TABLE;
typedef struct {
    uint8_t guid[16];
    void *table;
} EFI_CONFIGURATION_TABLE;
typedef EFI_STATUS(EFIAPI *allocate_pages_fn)(uint32_t, uint32_t, uint64_t, uint64_t *);
typedef EFI_STATUS(EFIAPI *allocate_pool_fn)(uint32_t, uint64_t, void **);
typedef EFI_STATUS(EFIAPI *exit_boot_services_fn)(EFI_HANDLE, uint64_t);

typedef EFI_STATUS(EFIAPI *locate_protocol_fn)(void *, void *, void **);

extern void knx_efi_handoff(uint64_t stack_top, uint64_t entry, void *info)
    __attribute__((noreturn));
typedef void(EFIAPI *output_string_fn)(void *, CHAR16 *);
typedef struct { uint16_t scan, unicode; } EFI_INPUT_KEY;
typedef EFI_STATUS(EFIAPI *read_key_fn)(void *, EFI_INPUT_KEY *);
typedef EFI_STATUS(EFIAPI *file_info_fn)(EFI_FILE *, void *, uint64_t *, void *);
typedef EFI_STATUS(EFIAPI *file_write_fn)(EFI_FILE *, uint64_t *, void *);
typedef EFI_STATUS(EFIAPI *file_set_info_fn)(EFI_FILE *, void *, uint64_t, void *);
static EFI_SYSTEM_TABLE *st;
static void **bs;

void *memset(void *destination, int value, unsigned long count) {
    unsigned char *bytes = destination;
    while (count--)
        *bytes++ = (unsigned char)value;
    return destination;
}

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static void serial_init(void) {
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 3);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
}

static void serial_puts(const char *s) {
    while (*s) {

        uint64_t deadline = rdtsc() + KNX_SERIAL_TX_TIMEOUT_TSC;
        while (!(inb(0x3fd) & 0x20) && rdtsc() < deadline) {
        }
        outb(0x3f8, (uint8_t)*s++);
    }
}
static void serial_hex(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4)
        serial_puts((char[]){h[(v >> i) & 15], 0});
}
static void print16(const char *s) {
    if (st && st->console_out) {
        CHAR16 b[128];
        unsigned n = 0;
        while (*s && n < 126)
            b[n++] = (uint8_t)*s++;
        b[n++] = 13;
        b[n++] = 10;
        b[n] = 0;
        ((output_string_fn)((void **)st->console_out)[1])(st->console_out, b);
    }
}
static void print16_hex(const char *label, uint64_t value) {
    char buffer[64];
    unsigned n = 0;
    while (*label && n < sizeof(buffer) - 1)
        buffer[n++] = *label++;
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0 && n < sizeof(buffer) - 1; shift -= 4)
        buffer[n++] = digits[(value >> shift) & 15];
    buffer[n] = 0;
    print16(buffer);
}

#if KNX_BOOT_TIMING

static uint64_t timing_last;
static void timing_start(void) { timing_last = rdtsc(); }
static void timing_mark(const char *label) {
    uint64_t now = rdtsc();
    uint64_t delta = now - timing_last;
    timing_last = now;
    serial_puts("[boot] ");
    serial_puts(label);
    serial_puts(" +");
    serial_hex(delta);
    serial_puts(" cyc / ");
    serial_hex(delta / KNX_TSC_PER_MS);
    serial_puts("ms\r\n");
}
#else
#define timing_start() ((void)0)
#define timing_mark(label) ((void)0)
#endif

static uint64_t boot_menu(void) {
    print16("KNX UEFI: booting default configuration (kernel console)");
    return 0;
}

static uint64_t kernel_entry_address = 0x100000;
__attribute__((noreturn)) static void fail(const char *msg) {
    serial_puts("KNX UEFI: ");
    serial_puts(msg);
    serial_puts("\r\n");
    print16(msg);
    for (;;)
        __asm__ volatile("cli; hlt");
}
static const uint8_t simple_fs_guid[16] = {0x22, 0x5b, 0x4e, 0x96, 0x59, 0x64, 0xd2, 0x11,
                                           0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};

static const uint8_t loaded_image_guid[16] = {
    0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
    0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};
static const uint8_t gop_guid[16] = {0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
                                     0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a};
static const uint8_t acpi2_guid[16] = {0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
                                       0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81};
static const uint8_t acpi1_guid[16] = {0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
                                       0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d};
static const uint8_t file_info_guid[16] = {0x92, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
                                           0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};

static EFI_SIMPLE_FS *boot_volume_fs(EFI_HANDLE image) {
    locate_protocol_fn handle_protocol = (locate_protocol_fn)bs[16];
    void *loaded = 0;
    if (handle_protocol(image, (void *)loaded_image_guid, &loaded))
        return 0;

    void *device = *(void **)((uint8_t *)loaded + 24);
    void *fs = 0;
    if (handle_protocol(device, (void *)simple_fs_guid, &fs))
        return 0;
    return (EFI_SIMPLE_FS *)fs;
}

static void dump_flight_log(EFI_FILE *root) {
    volatile struct knx_flight_header *h =
        (volatile struct knx_flight_header *)(uintptr_t)KNX_FLIGHT_PHYS;
    int have_log = h->magic == KNX_FLIGHT_MAGIC && h->len != 0;
    uint32_t total = have_log ? h->len : 0;
    if (total > KNX_FLIGHT_DATA)
        total = KNX_FLIGHT_DATA;
    static CHAR16 path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'B',
                            'O',  'O', 'T', 'L', 'O', 'G', '.',  'T', 'X', 'T', 0};
    EFI_FILE *file = 0;

    EFI_STATUS open_status = root->open(root, &file, path, 0x8000000000000003ULL, 0x20);
    if (open_status || !file) {
        serial_puts("KNX UEFI: cannot create BOOTLOG.TXT status 0x");
        serial_hex(open_status);
        serial_puts("\r\n");
        return;
    }

    uint8_t info[32];
    for (unsigned i = 0; i < sizeof(info); ++i)
        info[i] = 0;
    *(uint64_t *)info = sizeof(info);
    file_set_info_fn set_info = (file_set_info_fn)file->rest[4];
    if (set_info)
        (void)set_info(file, (void *)file_info_guid, sizeof(info), info);

    static uint8_t buffer[4096];
    file_write_fn write = (file_write_fn)file->rest[0];
    if (!have_log) {

        static const char msg[] =
            "KNX BOOTLOG: no previous kernel log (cold boot or kernel did not start)\r\n";
        for (unsigned i = 0; i < sizeof(msg) - 1; ++i)
            buffer[i] = (uint8_t)msg[i];
        uint64_t bytes = sizeof(msg) - 1;
        if (write)
            (void)write(file, &bytes, buffer);
        file->close(file);
        return;
    }
    uint32_t start = h->wrapped ? (h->len & (KNX_FLIGHT_DATA - 1)) : 0;
    for (uint32_t off = 0; off < total && write;) {
        uint32_t n = total - off;
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        for (uint32_t i = 0; i < n; ++i)
            buffer[i] = ((volatile uint8_t *)(h + 1))[(start + off + i) &
                                                       (KNX_FLIGHT_DATA - 1)];
        uint64_t bytes = n;
        if (write(file, &bytes, buffer) || !bytes)
            break;
        off += (uint32_t)bytes;
    }
    file->close(file);
    h->magic = 0;
    serial_puts("KNX UEFI: previous boot log saved to EFI/KNX/BOOTLOG.TXT\r\n");
}

static void setup_kernel_log(EFI_FILE *root, uint64_t *file_out,
                             uint64_t *write_out) {
    static CHAR16 path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'K',
                            'N',  'X', 'L', 'O', 'G', '.',  'T', 'X', 'T', 0};
    EFI_FILE *file = 0;
    if (root->open(root, &file, path, 0x8000000000000003ULL, 0x20) || !file)
        return;
    uint8_t info_buf[32];
    for (unsigned i = 0; i < sizeof(info_buf); ++i)
        info_buf[i] = 0;
    *(uint64_t *)info_buf = sizeof(info_buf);
    file_set_info_fn set_info = (file_set_info_fn)file->rest[4];
    if (set_info)
        (void)set_info(file, (void *)file_info_guid, sizeof(info_buf), info_buf);
    *file_out = (uint64_t)(uintptr_t)file;
    *write_out = (uint64_t)(uintptr_t)file->rest[0];
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    st = system;
    bs = (void **)((uint8_t *)system->boot_services + sizeof(EFI_TABLE_HEADER));
    serial_init();
    timing_start();
    serial_puts("KNX Boot Manager (UEFI)\r\n");
    timing_mark("banner");
    uint64_t boot_flags = boot_menu();
    timing_mark("menu");
    serial_puts("Loading EFI/KNX/KERNEL.BIN...\r\n");
    EFI_SIMPLE_FS *fs = boot_volume_fs(image);
    if (!fs) {

        EFI_STATUS fs_status =
            ((locate_protocol_fn)bs[37])((void *)simple_fs_guid, 0, (void **)&fs);
        if (fs_status || !fs) {
            serial_puts("LocateProtocol status 0x");
            serial_hex(fs_status);
            serial_puts("\r\n");
            fail("EFI system partition not found");
        }
    }
    print16("Boot volume ready");
    EFI_FILE *root = 0;
    if (((EFI_STATUS(EFIAPI *)(EFI_SIMPLE_FS *, EFI_FILE **))fs->open_volume)(fs, &root) || !root)
        fail("cannot open EFI volume");
    dump_flight_log(root);
    timing_mark("fs+log");

    static CHAR16 path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'K',
                            'E',  'R', 'N', 'E', 'L',  '.', 'B', 'I', 'N',  0};
    EFI_FILE *file = 0;
    if (root->open(root, &file, path, 1, 0) || !file)
        fail("EFI/KNX/KERNEL.BIN missing");
    uint8_t info_buf[512];
    for (unsigned i = 0; i < sizeof(info_buf); ++i)
        info_buf[i] = 0;
    uint64_t info_size = sizeof(info_buf);
    file_info_fn get_info = (file_info_fn)file->rest[3];
    if (!get_info || get_info(file, (void *)file_info_guid, &info_size, info_buf) ||
        info_size < 32)
        fail("cannot query the kernel image size");
    uint64_t kernel_size = *(uint64_t *)(info_buf + 8);
    if (!kernel_size || kernel_size > (64ULL << 20))
        fail("kernel image size is out of range");
    uint64_t pages = (kernel_size + 4095) / 4096;
    uint64_t addr = 0x100000;
    EFI_STATUS status = ((allocate_pages_fn)bs[2])(2, 2, pages, &addr);
    if (status || addr != 0x100000)
        fail("cannot reserve the kernel load address");
    uint8_t *dst = (uint8_t *)(uintptr_t)addr;
    uint64_t want = kernel_size;
    while (want) {
        uint64_t chunk = want;
        if (file->read(file, &chunk, dst) || !chunk)
            fail("cannot read kernel image");
        dst += chunk;
        want -= chunk;
    }
    file->close(file);
    print16("Kernel image loaded");
    timing_mark("kernel");
    uint64_t log_file = 0, log_write = 0;
    setup_kernel_log(root, &log_file, &log_write);
    print16(log_write ? "Live log: EFI/KNX/KNXLOG.TXT" : "Live log unavailable");
    timing_mark("log");
    root->close(root);

    uint64_t kernel_stack_pages = 16;

    uint64_t kernel_stack = 0x10000000;
    if (((allocate_pages_fn)bs[2])(2, 2, kernel_stack_pages, &kernel_stack) ||
        kernel_stack != 0x10000000)
        fail("cannot allocate kernel stack");
    print16("Kernel stack ready");

    EFI_MEMORY_DESCRIPTOR *mmap = 0;
    uint64_t mmap_capacity = 128 * 1024;
    if (((allocate_pool_fn)bs[5])(2, mmap_capacity, (void **)&mmap))
        fail("cannot reserve memory map buffer");
    knx_memory_region *regions = 0;
    uint64_t region_capacity = 2048;
    if (((allocate_pool_fn)bs[5])(2, region_capacity * sizeof(*regions), (void **)&regions))
        fail("cannot reserve normalized memory map");
    print16("Memory map ready");
    EFI_GOP *gop = 0;
    ((locate_protocol_fn)bs[37])((void *)gop_guid, 0, (void **)&gop);

    knx_boot_info graphics = {0};
    if (gop && gop->mode && gop->mode->info) {
        EFI_GOP_INFO *gi = gop->mode->info;
        uint64_t row_bytes = (uint64_t)gi->pixels_per_scanline * 4;
        uint64_t fb_bytes = row_bytes * gi->vres;
        if (gi->hres >= 8 && gi->vres >= 16 && gi->hres <= gi->pixels_per_scanline &&
            gi->hres <= 8192 && gi->vres <= 8192 && gi->pixels_per_scanline <= 8192 &&
            gi->pixel_format <= 2 && row_bytes / 4 == gi->pixels_per_scanline &&
            fb_bytes / gi->vres == row_bytes && gop->mode->fb_base &&
            gop->mode->fb_base <= UINT64_MAX - fb_bytes) {
            graphics.framebuffer_base = gop->mode->fb_base;

            graphics.framebuffer_size = fb_bytes;
            graphics.framebuffer_width = gi->hres;
            graphics.framebuffer_height = gi->vres;
            graphics.pixels_per_scanline = gi->pixels_per_scanline;
            graphics.pixel_format = gi->pixel_format;
            for (unsigned i = 0; i < 4; ++i)
                graphics.pixel_masks[i] = gi->pixel_info[i];
        }
    }
    print16_hex("GOP fb=", graphics.framebuffer_base);
    print16_hex(" GOP size=", graphics.framebuffer_size);
    print16_hex(" GOP w=", graphics.framebuffer_width);
    print16_hex(" h=", graphics.framebuffer_height);
    print16_hex(" pitch=", graphics.pixels_per_scanline);
    print16_hex(" fmt=", graphics.pixel_format);
    uint64_t acpi_rsdp = 0;
    EFI_CONFIGURATION_TABLE *config = system->configuration_table;
    for (uint64_t i = 0; i < system->table_entries; ++i) {
        unsigned j = 0;
        while (j < 16 && config[i].guid[j] == acpi2_guid[j])
            ++j;
        if (j == 16) {
            acpi_rsdp = (uint64_t)(uintptr_t)config[i].table;
            break;
        }
        j = 0;
        while (j < 16 && config[i].guid[j] == acpi1_guid[j])
            ++j;
        if (j == 16)
            acpi_rsdp = (uint64_t)(uintptr_t)config[i].table;
    }
    timing_mark("mmap");
    EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)mmap;
    uint64_t map_size = mmap_capacity, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;
    uint64_t count = 0;
    EFI_STATUS (*get_map)(uint64_t *, void *, uint64_t *, uint64_t *, uint32_t *) = (void *)bs[4];
    exit_boot_services_fn exit_bs = (exit_boot_services_fn)bs[26];
    print16("Exiting firmware services");
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        map_size = mmap_capacity;
        status = get_map(&map_size, mmap, &map_key, &desc_size, &desc_version);
        if (status)
            fail("GetMemoryMap failed");
        if (!desc_size || map_size / desc_size > region_capacity)
            fail("UEFI memory map is too large");
        count = 0;
        for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
            desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
            regions[count].base = desc->physical_start;
            regions[count].length = desc->pages * 4096;
            regions[count].type = desc->type == 7
                                      ? KNX_MEMORY_USABLE
                                      : (desc->type == 9 ? KNX_MEMORY_ACPI : KNX_MEMORY_RESERVED);
            regions[count].attributes = (uint32_t)desc->attribute;
            ++count;
        }
        status = exit_bs(image, map_key);
        if (!status)
            break;
        if (attempt == 3)
            fail("ExitBootServices failed");
    }
    timing_mark("exitbs");
    knx_boot_info info = {0};
    info.magic = KNX_BOOT_MAGIC;
    info.version = KNX_BOOT_VERSION;
    info.boot_method = KNX_BOOT_UEFI;
    info.acpi_rsdp = acpi_rsdp;

    info.boot_flags = boot_flags;
    info.log_file = log_file;
    info.log_write = log_write;
    info.memory_map = regions;
    info.memory_region_count = count;
    info.framebuffer_base = graphics.framebuffer_base;
    info.framebuffer_size = graphics.framebuffer_size;
    info.framebuffer_width = graphics.framebuffer_width;
    info.framebuffer_height = graphics.framebuffer_height;
    info.pixels_per_scanline = graphics.pixels_per_scanline;
    info.pixel_format = graphics.pixel_format;
    for (unsigned i = 0; i < 4; ++i)
        info.pixel_masks[i] = graphics.pixel_masks[i];
    timing_mark("handoff");
    serial_puts("KNX UEFI: exiting firmware services\r\n");
    print16_hex("Kernel entry=", kernel_entry_address);
    print16_hex(" Kernel first word=",
                *(volatile uint64_t *)(uintptr_t)kernel_entry_address);
    print16_hex(" Stack=", kernel_stack + kernel_stack_pages * 4096);
    print16_hex(" Info=", (uint64_t)(uintptr_t)&info);
    print16("HANDOFF: jumping to kernel");
    knx_efi_handoff(kernel_stack + kernel_stack_pages * 4096,
                    kernel_entry_address, &info);
}
