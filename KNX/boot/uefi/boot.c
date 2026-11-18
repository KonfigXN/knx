#include <stdint.h>
#include "knx.h"

/*
 * AArch64 UEFI loader.
 *
 * UEFI on AArch64 uses the standard AAPCS64 calling convention for every
 * protocol call, so EFIAPI is empty.  Timing comes from the architectural
 * counter and control is transferred to the kernel through a normal function
 * call; no assembly handoff layer is needed.
 */
#define EFIAPI

/* ---- Minimal UEFI types ------------------------------------------------- */

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
typedef EFI_STATUS(EFIAPI *free_pages_fn)(uint64_t, uint64_t);
typedef EFI_STATUS(EFIAPI *allocate_pool_fn)(uint32_t, uint64_t, void **);
typedef EFI_STATUS(EFIAPI *get_memory_map_fn)(uint64_t *, void *, uint64_t *, uint64_t *, uint32_t *);
typedef EFI_STATUS(EFIAPI *exit_boot_services_fn)(EFI_HANDLE, uint64_t);
typedef EFI_STATUS(EFIAPI *locate_protocol_fn)(void *, void *, void **);
typedef EFI_STATUS(EFIAPI *output_string_fn)(void *, CHAR16 *);
typedef EFI_STATUS(EFIAPI *file_info_fn)(EFI_FILE *, void *, uint64_t *, void *);

/* Boot Services table slots (after EFI_TABLE_HEADER). */
enum {
    BS_ALLOCATE_PAGES = 2,
    BS_FREE_PAGES = 3,
    BS_GET_MEMORY_MAP = 4,
    BS_ALLOCATE_POOL = 5,
    BS_HANDLE_PROTOCOL = 16,
    BS_EXIT_BOOT_SERVICES = 26,
    BS_LOCATE_PROTOCOL = 37,
};

enum {
    EFI_ALLOCATE_ANY_PAGES = 0,
    EFI_ALLOCATE_ADDRESS = 2,
};

enum {
    EFI_LOADER_DATA = 2,
    EFI_CONVENTIONAL_MEMORY = 7,
    EFI_ACPI_RECLAIM_MEMORY = 9,
    EFI_ACPI_MEMORY_NVS = 10,
};

#define EFI_FILE_MODE_READ 1u
#define KNX_PAGE_SIZE 4096u
#define KNX_MAX_IMAGE_SIZE (64ULL << 20)
#define KNX_MAX_PHDRS 16u

/* ---- ELF64 (little endian, EM_AARCH64) ---------------------------------- */

typedef struct {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} Elf64_Phdr;

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define EM_AARCH64 183u
#define PT_LOAD 1u

static EFI_SYSTEM_TABLE *st;
static void **bs;

/* Provided because compiler-generated zero-initialisations of large objects
 * emit calls to memset; the UEFI image is linked with -nodefaultlib. */
void *memset(void *destination, int value, unsigned long count) {
    unsigned char *bytes = destination;
    while (count--)
        *bytes++ = (unsigned char)value;
    return destination;
}

/* ---- GUIDs -------------------------------------------------------------- */

static const uint8_t simple_fs_guid[16] = {0x22, 0x5b, 0x4e, 0x96, 0x59, 0x64, 0xd2, 0x11,
                                           0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};
static const uint8_t loaded_image_guid[16] = {0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
                                              0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};
static const uint8_t gop_guid[16] = {0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
                                     0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a};
static const uint8_t acpi2_guid[16] = {0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
                                       0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81};
static const uint8_t acpi1_guid[16] = {0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
                                       0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d};
static const uint8_t file_info_guid[16] = {0x92, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
                                           0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};

static int guid_equal(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 16; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ---- Debug output via ConOut -------------------------------------------- */

static void print16(const char *s) {
    if (!st || !st->console_out)
        return;
    CHAR16 text[160];
    unsigned n = 0;
    while (*s && n + 2 < sizeof(text) / sizeof(text[0]))
        text[n++] = (uint8_t)*s++;
    text[n++] = 13;
    text[n++] = 10;
    text[n] = 0;
    ((output_string_fn)((void **)st->console_out)[1])(st->console_out, text);
}

static void print16_hex(const char *label, uint64_t value) {
    char buffer[80];
    unsigned n = 0;
    while (*label && n < sizeof(buffer) - 1)
        buffer[n++] = *label++;
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0 && n < sizeof(buffer) - 1; shift -= 4)
        buffer[n++] = digits[(value >> shift) & 15];
    buffer[n] = 0;
    print16(buffer);
}

__attribute__((noreturn)) static void fail(const char *msg) {
    print16("KNX UEFI: ");
    print16(msg);
    for (;;)
        __asm__ volatile("wfi");
}

static uint64_t boot_flags(void) {
    print16("KNX AArch64 UEFI: booting kernel console configuration");
    return KNX_BOOT_CONSOLE;
}

/* ---- Filesystem helpers ------------------------------------------------- */

static EFI_SIMPLE_FS *boot_volume_fs(EFI_HANDLE image) {
    void *loaded = 0;
    if (((locate_protocol_fn)bs[BS_HANDLE_PROTOCOL])(image, (void *)loaded_image_guid, &loaded))
        return 0;

    /* EFI_LOADED_IMAGE.DeviceHandle lives at byte offset 24. */
    void *device = *(void **)((uint8_t *)loaded + 24);
    void *fs = 0;
    if (((locate_protocol_fn)bs[BS_HANDLE_PROTOCOL])(device, (void *)simple_fs_guid, &fs))
        return 0;
    return (EFI_SIMPLE_FS *)fs;
}

static EFI_STATUS open_file(EFI_FILE *root, CHAR16 *path, EFI_FILE **file_out,
                            uint64_t *size_out) {
    EFI_FILE *file = 0;
    EFI_STATUS status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status || !file)
        return status ? status : 1;

    uint8_t info_buf[256] __attribute__((aligned(8)));
    for (unsigned i = 0; i < sizeof(info_buf); ++i)
        info_buf[i] = 0;
    uint64_t info_size = sizeof(info_buf);
    file_info_fn get_info = (file_info_fn)file->rest[3];
    if (!get_info || get_info(file, (void *)file_info_guid, &info_size, info_buf) ||
        info_size < 32) {
        file->close(file);
        return 1;
    }
    uint64_t size = *(uint64_t *)(info_buf + 8);
    if (!size || size > KNX_MAX_IMAGE_SIZE) {
        file->close(file);
        return 1;
    }
    *file_out = file;
    *size_out = size;
    return 0;
}

static int read_into(EFI_FILE *file, void *dst, uint64_t count) {
    uint8_t *bytes = dst;
    uint64_t remaining = count;
    while (remaining) {
        uint64_t chunk = remaining;
        if (file->read(file, &chunk, bytes) || !chunk)
            return 1;
        bytes += chunk;
        remaining -= chunk;
    }
    return 0;
}

static EFI_STATUS read_whole_file(EFI_FILE *root, CHAR16 *path, uint64_t **data_out,
                                  uint64_t *size_out) {
    EFI_FILE *file = 0;
    uint64_t size = 0;
    if (open_file(root, path, &file, &size))
        return 1;

    uint64_t pages = (size + KNX_PAGE_SIZE - 1) / KNX_PAGE_SIZE;
    uint64_t addr = 0;
    if (((allocate_pages_fn)bs[BS_ALLOCATE_PAGES])(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                                   pages, &addr)) {
        file->close(file);
        return 1;
    }
    if (read_into(file, (void *)(uintptr_t)addr, size)) {
        ((free_pages_fn)bs[BS_FREE_PAGES])(pages, addr);
        file->close(file);
        return 1;
    }
    file->close(file);
    *data_out = (uint64_t *)addr;
    *size_out = size;
    return 0;
}

/* ---- EFI entry point ---------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    st = system;
    bs = (void **)((uint8_t *)system->boot_services + sizeof(EFI_TABLE_HEADER));

    uint64_t flags = boot_flags();

    EFI_SIMPLE_FS *fs = boot_volume_fs(image);
    if (!fs)
        ((locate_protocol_fn)bs[BS_LOCATE_PROTOCOL])((void *)simple_fs_guid, 0, (void **)&fs);
    if (!fs)
        fail("EFI system partition not found");

    EFI_FILE *root = 0;
    if (((EFI_STATUS(EFIAPI *)(EFI_SIMPLE_FS *, EFI_FILE **))fs->open_volume)(fs, &root) || !root)
        fail("cannot open EFI volume");

    static CHAR16 kernel_bin_path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'K',
                                       'E',  'R', 'N', 'E', 'L', '.',  'B', 'I', 'N', 0};
    static CHAR16 kernel_elf_path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'K',
                                       'E',  'R', 'N', 'E', 'L', '.',  'E', 'L', 'F', 0};
    static CHAR16 user_img_path[] = {'\\', 'E', 'F', 'I', '\\', 'K', 'N', 'X', '\\', 'U',
                                     'S',  'E', 'R', '.',  'I', 'M', 'G', 0};

    EFI_FILE *kernel_file = 0;
    CHAR16 *kernel_path = kernel_bin_path;
    uint64_t elf_size = 0;
    if (open_file(root, kernel_bin_path, &kernel_file, &elf_size)) {
        kernel_path = kernel_elf_path;
        if (open_file(root, kernel_elf_path, &kernel_file, &elf_size))
            fail("EFI/KNX/KERNEL.BIN or KERNEL.ELF missing");
    }

    /*
     * Read just the ELF header into a scratch page first.  Reserving each
     * PT_LOAD at its physical address before allocating the full file buffer
     * guarantees that the file buffer cannot overlap the kernel image.
     */
    uint64_t scratch = 0;
    if (((allocate_pages_fn)bs[BS_ALLOCATE_PAGES])(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                                   1, &scratch))
        fail("cannot allocate ELF header buffer");
    uint64_t header_bytes = elf_size < KNX_PAGE_SIZE ? elf_size : KNX_PAGE_SIZE;
    if (read_into(kernel_file, (void *)(uintptr_t)scratch, header_bytes))
        fail("cannot read ELF header");
    kernel_file->close(kernel_file);

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)(uintptr_t)scratch;
    if (header_bytes < sizeof(*ehdr) || ehdr->ident[0] != 0x7f || ehdr->ident[1] != 'E' ||
        ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F' || ehdr->ident[4] != ELFCLASS64 ||
        ehdr->ident[5] != ELFDATA2LSB || ehdr->ident[6] != EV_CURRENT ||
        ehdr->machine != EM_AARCH64 || !ehdr->phnum ||
        ehdr->phentsize != sizeof(Elf64_Phdr) ||
        ehdr->phoff > elf_size || ehdr->phoff > header_bytes ||
        ehdr->phnum > KNX_MAX_PHDRS ||
        ehdr->phnum > (header_bytes - ehdr->phoff) / ehdr->phentsize)
        fail("malformed little-endian AArch64 ELF kernel image");

    uint16_t phnum = ehdr->phnum;
    uint64_t kernel_entry = ehdr->entry;
    Elf64_Phdr phdrs[KNX_MAX_PHDRS];
    for (uint16_t i = 0; i < phnum; ++i)
        phdrs[i] = ((Elf64_Phdr *)(uintptr_t)(scratch + ehdr->phoff))[i];

    for (uint16_t i = 0; i < phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->type != PT_LOAD)
            continue;
        if (ph->filesz > ph->memsz || ph->offset > elf_size ||
            ph->filesz > elf_size - ph->offset || !ph->memsz)
            fail("invalid ELF PT_LOAD segment");
        uint64_t paddr = ph->paddr ? ph->paddr : ph->vaddr;
        if (paddr > (1ULL << 48) || ph->memsz > (1ULL << 48) - paddr)
            fail("invalid ELF PT_LOAD address range");
        uint64_t start = paddr & ~(uint64_t)(KNX_PAGE_SIZE - 1);
        uint64_t end = (paddr + ph->memsz + KNX_PAGE_SIZE - 1) &
                       ~(uint64_t)(KNX_PAGE_SIZE - 1);
        if (end <= start || end > (1ULL << 48))
            fail("invalid ELF PT_LOAD address range");
        uint64_t pages = (end - start) / KNX_PAGE_SIZE;
        uint64_t addr = start;
        if (((allocate_pages_fn)bs[BS_ALLOCATE_PAGES])(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                                       pages, &addr) ||
            addr != start)
            fail("cannot reserve kernel segment at its physical address");
        uint64_t *words = (uint64_t *)(uintptr_t)start;
        uint64_t count = (end - start) / sizeof(uint64_t);
        while (count--)
            *words++ = 0;
    }
    ((free_pages_fn)bs[BS_FREE_PAGES])(1, scratch);

    uint64_t *elf_image = 0;
    if (read_whole_file(root, kernel_path, &elf_image, &elf_size) || !elf_image)
        fail("cannot read the kernel image");
    uint8_t *elf_bytes = (uint8_t *)(uintptr_t)elf_image;
    for (uint16_t i = 0; i < phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->type != PT_LOAD || !ph->filesz)
            continue;
        uint64_t paddr = ph->paddr ? ph->paddr : ph->vaddr;
        for (uint64_t off = 0; off < ph->filesz; ++off)
            ((uint8_t *)(uintptr_t)paddr)[off] = elf_bytes[ph->offset + off];
    }

    uint64_t initrd_base = 0, initrd_size = 0;
    {
        uint64_t *initrd = 0;
        uint64_t image_size = 0;
        if (!read_whole_file(root, user_img_path, &initrd, &image_size)) {
            initrd_base = (uint64_t)(uintptr_t)initrd;
            initrd_size = image_size;
        }
    }
    root->close(root);

    /* ACPI RSDP from the configuration table. */
    uint64_t acpi_rsdp = 0;
    EFI_CONFIGURATION_TABLE *config = system->configuration_table;
    for (uint64_t i = 0; i < system->table_entries; ++i) {
        if (guid_equal(config[i].guid, acpi2_guid)) {
            acpi_rsdp = (uint64_t)(uintptr_t)config[i].table;
            break;
        }
        if (guid_equal(config[i].guid, acpi1_guid))
            acpi_rsdp = (uint64_t)(uintptr_t)config[i].table;
    }

    /* Boot information must survive ExitBootServices. */
    uint64_t info_addr = 0;
    if (((allocate_pages_fn)bs[BS_ALLOCATE_PAGES])(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                                   1, &info_addr))
        fail("cannot allocate boot information page");
    knx_boot_info *info = (knx_boot_info *)(uintptr_t)info_addr;
    memset(info, 0, sizeof(*info));
    info->magic = KNX_BOOT_MAGIC;
    info->version = KNX_BOOT_VERSION;
    info->boot_method = KNX_BOOT_UEFI;
    info->boot_flags = flags;
    info->acpi_rsdp = acpi_rsdp;
    info->initrd_base = initrd_base;
    info->initrd_size = initrd_size;

    /* Framebuffer description from GOP. */
    EFI_GOP *gop = 0;
    ((locate_protocol_fn)bs[BS_LOCATE_PROTOCOL])((void *)gop_guid, 0, (void **)&gop);
    if (gop && gop->mode && gop->mode->info) {
        EFI_GOP_INFO *gi = gop->mode->info;
        uint64_t row_bytes = (uint64_t)gi->pixels_per_scanline * 4;
        uint64_t fb_bytes = row_bytes * gi->vres;
        if (gi->hres >= 8 && gi->vres >= 16 && gi->hres <= gi->pixels_per_scanline &&
            gi->hres <= 8192 && gi->vres <= 8192 && gi->pixels_per_scanline <= 8192 &&
            gi->pixel_format <= 2 && row_bytes / 4 == gi->pixels_per_scanline &&
            fb_bytes / gi->vres == row_bytes && gop->mode->fb_base &&
            gop->mode->fb_base <= UINT64_MAX - fb_bytes) {
            info->framebuffer_base = gop->mode->fb_base;
            info->framebuffer_size = fb_bytes;
            info->framebuffer_width = gi->hres;
            info->framebuffer_height = gi->vres;
            info->pixels_per_scanline = gi->pixels_per_scanline;
            info->pixel_format = gi->pixel_format;
            for (unsigned i = 0; i < 4; ++i)
                info->pixel_masks[i] = gi->pixel_info[i];
        }
    }

    /* Private contiguous stack reservation.  knx_entry sets its own stack in
     * entry.S, so no assembly handoff is required; the reservation simply
     * keeps these pages out of the kernel's free-memory pool. */
    uint64_t kernel_stack = 0;
    if (((allocate_pages_fn)bs[BS_ALLOCATE_PAGES])(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                                   16, &kernel_stack))
        fail("cannot reserve kernel stack");

    EFI_MEMORY_DESCRIPTOR *mmap = 0;
    uint64_t mmap_capacity = 128 * 1024;
    if (((allocate_pool_fn)bs[BS_ALLOCATE_POOL])(EFI_LOADER_DATA, mmap_capacity, (void **)&mmap))
        fail("cannot reserve memory map buffer");
    knx_memory_region *regions = 0;
    uint64_t region_capacity = 2048;
    if (((allocate_pool_fn)bs[BS_ALLOCATE_POOL])(EFI_LOADER_DATA,
                                                 region_capacity * sizeof(*regions),
                                                 (void **)&regions))
        fail("cannot reserve normalized memory map");

    /* All firmware interaction is complete: log before ExitBootServices. */
    print16_hex("GOP fb=", info->framebuffer_base);
    print16_hex(" size=", info->framebuffer_size);
    print16_hex(" RSDP=", info->acpi_rsdp);
    print16_hex(" Initrd=", info->initrd_base);
    print16_hex(" Kernel entry=", kernel_entry);
    print16_hex(" Stack=", kernel_stack + 16 * KNX_PAGE_SIZE);
    print16("Exiting UEFI boot services");

    get_memory_map_fn get_map = (get_memory_map_fn)bs[BS_GET_MEMORY_MAP];
    exit_boot_services_fn exit_bs = (exit_boot_services_fn)bs[BS_EXIT_BOOT_SERVICES];
    uint64_t map_key = 0, desc_size = 0, region_count = 0;
    uint64_t total_memory = 0, available_memory = 0;
    uint32_t desc_version = 0;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint64_t map_size = mmap_capacity;
        EFI_STATUS status = get_map(&map_size, mmap, &map_key, &desc_size, &desc_version);
        if (status)
            fail("GetMemoryMap failed");
        if (!desc_size || map_size / desc_size > region_capacity)
            fail("UEFI memory map is too large");

        total_memory = 0;
        available_memory = 0;
        region_count = 0;
        for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
            uint64_t length = desc->pages * KNX_PAGE_SIZE;
            uint32_t type = KNX_MEMORY_RESERVED;
            if (desc->type == EFI_CONVENTIONAL_MEMORY) {
                type = KNX_MEMORY_USABLE;
                available_memory += length;
            } else if (desc->type == EFI_ACPI_RECLAIM_MEMORY ||
                       desc->type == EFI_ACPI_MEMORY_NVS) {
                type = KNX_MEMORY_ACPI;
            }
            total_memory += length;
            regions[region_count].base = desc->physical_start;
            regions[region_count].length = length;
            regions[region_count].type = type;
            regions[region_count].attributes = (uint32_t)desc->attribute;
            ++region_count;
        }
        if (!exit_bs(image, map_key))
            break;
        if (attempt == 3)
            fail("ExitBootServices failed");
    }

    info->memory_map = regions;
    info->memory_region_count = region_count;
    info->total_memory = total_memory;
    info->available_memory = available_memory;

    /* The MMU is still on (EDK2 identity maps memory); call the AArch64 kernel
     * entry directly with x0 = info.  entry.S installs the kernel stack and,
     * if needed, drops from EL2 to EL1 itself. */
    typedef void (*kernel_entry_fn)(knx_boot_info *);
    ((kernel_entry_fn)(uintptr_t)kernel_entry)(info);

    for (;;)
        __asm__ volatile("wfi");
}
