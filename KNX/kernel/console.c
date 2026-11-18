#include "knx.h"
static volatile uint16_t *const vga = (uint16_t *)0xb8000;
static unsigned vga_cursor;
static unsigned col, row;
static const uint8_t font[36][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},   {14, 16, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
static const uint8_t lowercase_font[26][7] = {
    {0,14,1,15,17,15,0}, {16,16,30,17,17,30,0}, {0,14,16,16,16,14,0},
    {1,1,15,17,17,15,0}, {0,14,17,31,16,14,0}, {6,8,30,8,8,8,0},
    {0,15,17,15,1,14,0}, {16,16,30,17,17,17,0}, {4,0,12,4,4,14,0},
    {2,0,6,2,18,12,0}, {16,16,18,20,24,20,0}, {12,4,4,4,4,14,0},
    {0,26,21,21,21,21,0}, {0,30,17,17,17,17,0}, {0,14,17,17,17,14,0},
    {0,30,17,30,16,16,0}, {0,15,17,15,1,1,0}, {0,22,24,16,16,16,0},
    {0,15,16,14,1,30,0}, {8,8,30,8,8,6,0}, {0,17,17,17,19,13,0},
    {0,17,17,17,10,4,0}, {0,17,17,21,21,10,0}, {0,17,10,4,10,17,0},
    {0,17,17,15,1,14,0}, {0,31,2,4,8,31,0}};

static const char punctuation_chars[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static const uint8_t punctuation_font[32][7] = {
    {4,4,4,4,4,0,4}, {10,10,10,0,0,0,0}, {10,31,10,10,31,10,0},
    {4,15,20,14,5,30,4}, {24,25,2,4,8,19,3}, {12,18,20,8,21,18,13},
    {4,4,8,0,0,0,0}, {2,4,8,8,8,4,2}, {8,4,2,2,2,4,8},
    {0,21,14,31,14,21,0}, {0,4,4,31,4,4,0}, {0,0,0,0,0,4,8},
    {0,0,0,31,0,0,0}, {0,0,0,0,0,12,12}, {1,2,2,4,8,8,16},
    {0,12,12,0,12,12,0}, {0,12,12,0,4,8,0}, {2,4,8,16,8,4,2},
    {0,31,0,31,0,0,0}, {8,4,2,1,2,4,8}, {14,17,1,2,4,0,4},
    {14,17,23,21,23,16,14}, {14,8,8,8,8,8,14}, {16,8,8,4,2,2,1},
    {14,2,2,2,2,2,14}, {4,10,17,0,0,0,0}, {0,0,0,0,0,31,0},
    {8,4,2,0,0,0,0}, {2,4,4,8,4,4,2}, {4,4,4,4,4,4,4},
    {8,4,4,2,4,4,8}, {0,0,9,22,0,0,0}};
static unsigned fb_width, fb_height;
static int framebuffer_active;
static int serial_only;
static uint64_t boot_flags;

static uint64_t log_file_handle, log_write_fn;
static char log_buffer[128];
static unsigned log_len;

static void log_file_flush(void) {
    if (!log_write_fn || !log_file_handle || !log_len)
        return;
    typedef uint64_t (*efi_file_write_fn)(void *, uint64_t *, void *);
    typedef uint64_t (*efi_file_flush_fn)(void *);
    uint64_t bytes = log_len;
    efi_file_write_fn write = (efi_file_write_fn)log_write_fn;
    if (write((void *)(uintptr_t)log_file_handle, &bytes, log_buffer) || !bytes) {
        log_write_fn = 0;
    } else {

        efi_file_flush_fn flush = *(efi_file_flush_fn *)((uint8_t *)log_file_handle + 40 + 5 * 8);
        if (flush)
            (void)flush((void *)(uintptr_t)log_file_handle);
    }
    log_len = 0;
}

uint64_t knx_boot_flags(void) {
    return boot_flags;
}

static volatile struct knx_flight_header *const flight =
    (volatile struct knx_flight_header *)(uintptr_t)KNX_FLIGHT_PHYS;

static inline void flight_flush(volatile void *p) {
    __asm__ volatile("clflush %0" : : "m"(*(volatile uint8_t *)p) : "memory");
}

void knx_flight_init(void) {
    flight->len = 0;
    flight->wrapped = 0;
    flight->magic = KNX_FLIGHT_MAGIC;
    flight_flush(&flight->magic);
    flight_flush(&flight->len);
    flight_flush(&flight->wrapped);
}
void knx_flight_putc(char c) {
    if (flight->magic != KNX_FLIGHT_MAGIC)
        return;
    uint32_t pos = __sync_fetch_and_add(&flight->len, 1);
    volatile uint8_t *slot =
        &((volatile uint8_t *)(flight + 1))[pos & (KNX_FLIGHT_DATA - 1)];
    *slot = (uint8_t)c;
    flight_flush(slot);
    flight_flush(&flight->len);
    if (pos >= KNX_FLIGHT_DATA) {
        flight->wrapped = 1;
        flight_flush(&flight->wrapped);
    }
}

uint32_t knx_flight_read(uint8_t *dst, uint32_t max) {
    if (flight->magic != KNX_FLIGHT_MAGIC || !dst || !max)
        return 0;
    uint32_t total = flight->len;
    if (!total)
        return 0;
    if (total > KNX_FLIGHT_DATA)
        total = KNX_FLIGHT_DATA;
    uint32_t start = flight->wrapped ? (flight->len & (KNX_FLIGHT_DATA - 1)) : 0;
    if (max < total) {
        start = (start + total - max) & (KNX_FLIGHT_DATA - 1);
        total = max;
    }
    for (uint32_t i = 0; i < total; ++i)
        dst[i] = ((volatile uint8_t *)(flight + 1))[(start + i) & (KNX_FLIGHT_DATA - 1)];
    return total;
}

static int graphics_owner;

#define CON_MAX_COLS 512u
#define CON_MAX_ROWS 160u

static char *con_text;
static unsigned con_cols, con_rows;
static unsigned con_used;
static uint32_t con_bg, con_fg;

static uint8_t glyph_row(unsigned char c, unsigned y) {
    if (c >= 'a' && c <= 'z')
        return lowercase_font[c - 'a'][y];
    if (c >= 'A' && c <= 'Z')
        return font[c - 'A'][y];
    if (c >= '0' && c <= '9')
        return font[26 + c - '0'][y];
    for (unsigned i = 0; i < sizeof(punctuation_chars) - 1; ++i)
        if (c == (unsigned char)punctuation_chars[i])
            return punctuation_font[i][y];
    if (c == ' ')
        return 0;
    return punctuation_font[20][y];
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

static void serial_char(char c) {
    for (unsigned spin = 0; spin < 200000u; ++spin) {
        if (inb(0x3fd) & 0x20) {
            outb(0x3f8, (uint8_t)c);
            return;
        }
        (void)inb(0x3fd);
    }
}

char knx_serial_read_raw(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) : : "memory");
    __asm__ volatile("cli" : : : "memory");
    char c = 0;
    uint8_t lsr = inb(0x3fd);

    if (lsr != 0xff && (lsr & 1))
        c = (char)inb(0x3f8);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
    return c;
}
char knx_serial_read_char(void) {
    char c = knx_serial_read_raw();
    if (!c)
        return 0;
    if (c == '\r')
        return '\n';

    if ((uint8_t)c == 0x08 || (uint8_t)c == 0x7f)
        return '\b';
    return c;
}

static void draw_cell(unsigned cx, unsigned cy, unsigned char c) {
    uint32_t pix[16][8];
    for (unsigned y = 0; y < 16; ++y) {
        unsigned bits = 0;
        if (y >= 1 && y <= 14)
            bits = glyph_row(c, (y - 1) / 2);
        for (unsigned x = 0; x < 8; ++x) {
            unsigned on = x >= 1 && x <= 5 && ((bits >> (5 - x)) & 1u);
            pix[y][x] = on ? con_fg : con_bg;
        }
    }
    knx_gfx_blit(cx * 8, cy * 16, &pix[0][0], 8, 16, 8);
}

static void redraw_console(void) {
    if (con_text && con_used && con_cols && con_rows)
        for (unsigned y = 0; y < con_rows; ++y)
            for (unsigned x = 0; x < con_used; ++x)
                draw_cell(x, y, (unsigned char)con_text[y * con_cols + x]);
}

static void scroll_console(void) {
    if (con_text) {
        for (unsigned y = 1; y < con_rows; ++y)
            for (unsigned x = 0; x < con_cols; ++x)
                con_text[(y - 1) * con_cols + x] = con_text[y * con_cols + x];
        for (unsigned x = 0; x < con_cols; ++x)
            con_text[(con_rows ? con_rows - 1 : 0) * con_cols + x] = 0;
    }
    row = con_rows ? con_rows - 1 : 0;
    redraw_console();
}

static void draw_char(char c) {
    if (!framebuffer_active || graphics_owner || !con_text || !con_cols || !con_rows)
        return;
    if (c == '\b') {
        if (col)
            --col;
        return;
    }
    if (c == '\n') {
        col = 0;
        if (row + 1 >= con_rows)
            scroll_console();
        else
            ++row;
        return;
    }
    unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc > 126)
        uc = ' ';
    if (col >= con_cols) {
        col = 0;
        if (row + 1 >= con_rows)
            scroll_console();
        else
            ++row;
    }
    con_text[row * con_cols + col] = (char)uc;
    if (col + 1 > con_used)
        con_used = col + 1;
    draw_cell(col, row, uc);
    if (++col >= con_cols) {
        col = 0;
        if (row + 1 >= con_rows)
            scroll_console();
        else
            ++row;
    }
}
void knx_set_boot_info(knx_boot_info *info) {
    boot_flags = info ? info->boot_flags : 0;
    log_file_handle = 0;

    log_write_fn = 0;
    framebuffer_active = knx_graphics_init(info);

    knx_puts("[GFX] fb=0x");
    knx_print_u64(info ? info->framebuffer_base : 0);
    knx_puts(" size=");
    knx_print_u64(info ? info->framebuffer_size : 0);
    knx_puts(" geom=");
    knx_print_u64(info ? info->framebuffer_width : 0);
    knx_putc('x');
    knx_print_u64(info ? info->framebuffer_height : 0);
    knx_puts(" pitch=");
    knx_print_u64(info ? info->pixels_per_scanline : 0);
    knx_puts(" fmt=");
    knx_print_u64(info ? info->pixel_format : 0);
    knx_puts(" init=");
    knx_puts(framebuffer_active ? "OK\n" : "FAILED\n");
    if (framebuffer_active) {
        fb_width = info->framebuffer_width;
        fb_height = info->framebuffer_height;
        col = row = 0;
        con_cols = fb_width / 8;
        con_rows = fb_height / 16;
        if (con_cols > CON_MAX_COLS)
            con_cols = CON_MAX_COLS;
        if (con_rows > CON_MAX_ROWS)
            con_rows = CON_MAX_ROWS;
        con_used = 0;
        con_bg = knx_gfx_encode_color(0x101820);
        con_fg = knx_gfx_encode_color(0x00ffffff);
        size_t cells = (size_t)con_cols * con_rows;
        size_t pages = (cells + 4095u) / 4096u;
        con_text = knx_alloc_contiguous_pages(pages);
        if (con_text) {
            for (size_t i = 0; i < cells; ++i)
                con_text[i] = 0;
        } else {

            con_cols = con_rows = 0;
            serial_only = 1;
        }
        knx_gfx_fill_rect(0, 0, fb_width, fb_height, 0x101820);
    } else {
        fb_width = fb_height = 0;
        con_cols = con_rows = 0;
    }
    if (boot_flags & KNX_BOOT_CONSOLE) {
        framebuffer_active = 0;
        fb_width = fb_height = 0;
        con_cols = con_rows = 0;
        serial_only = 1;
    }
}
static void serial_init(void) {
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 3);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
}
void knx_console_early_init(void) {
    serial_only = 1;
    serial_init();
    knx_flight_init();
}
void knx_console_init(knx_boot_info *info) {
    serial_init();
    knx_set_boot_info(info);
    serial_only = 0;
}

static void log_line(const char *category, const char *message) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile("cli" : : : "memory");
    knx_puts(category);
    knx_puts(" ");
    knx_puts(message);
    knx_puts("\n");
    __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
}
void knx_log(const char *category, const char *message) {
    if (!category || !message)
        return;
#ifdef KNX_QUIET_LOGS

    if (boot_flags & KNX_BOOT_VERBOSE) {
        log_line(category, message);
        return;
    }

    int keep = (category[0]=='[' && category[1]=='E' && category[2]=='R' && category[3]=='R') ||
        (category[0]=='[' && category[1]=='W' && category[2]=='A' && category[3]=='R') ||
        (category[0]=='[' && category[1]=='R' && category[2]=='O' && category[3]=='O') ||
        (category[0]=='[' && category[1]=='I' && category[2]=='N' && category[3]=='I') ||
        (category[0]=='[' && category[1]=='T' && category[2]=='U' && category[3]=='I') ||
        (category[0]=='[' && category[1]=='T' && category[2]=='E' && category[3]=='S');
    if (!keep)
        return;
#endif
    log_line(category, message);
}
void knx_putc(char c) {

    if (log_write_fn && log_file_handle && log_len < sizeof(log_buffer) - 2) {
        if (c == '\n' && log_len && log_buffer[log_len - 1] != '\r')
            log_buffer[log_len++] = '\r';
        log_buffer[log_len++] = c;
        if (c == '\n' || log_len >= sizeof(log_buffer) - 2)
            log_file_flush();
    }
    knx_flight_putc(c);
    int slot = knx_task_cap_slot();
    if (slot >= 0) {
        knx_capture_putc(slot, c);
        return;
    }
    serial_char(c);
    if (framebuffer_active) {
        draw_char(c);
        return;
    }
    if (serial_only)
        return;

    if (c == '\b') {
        if (vga_cursor)
            --vga_cursor;
        return;
    }
    if (c == '\n') {
        if (vga_cursor / 80 >= 24) {
            for (unsigned y = 0; y < 24; ++y)
                for (unsigned x = 0; x < 80; ++x)
                    vga[y * 80 + x] = vga[(y + 1) * 80 + x];
            for (unsigned x = 0; x < 80; ++x)
                vga[24 * 80 + x] = 0x0f20;
            vga_cursor = 24 * 80;
        } else
            vga_cursor = ((vga_cursor / 80) + 1) * 80;
        return;
    }
    if (vga_cursor < 2000)
        vga[vga_cursor++] = (uint16_t)(0x0f00 | (uint8_t)c);
}
void knx_puts(const char *s) {
    while (*s)
        knx_putc(*s++);
}
void knx_console_graphics_owner(void) {
    graphics_owner = 1;
}
void knx_clear(void) {
    if (framebuffer_active && !graphics_owner) {
        knx_gfx_fill_rect(0, 0, fb_width, fb_height, 0x101820);
        col = row = 0;
        con_used = 0;
        if (con_text)
            for (size_t i = 0; i < (size_t)con_cols * con_rows; ++i)
                con_text[i] = 0;
        return;
    }
    for (unsigned i = 0; i < 2000; ++i)
        vga[i] = 0x0f20;
    vga_cursor = 0;
    static const char clear[] = {27, '[', '2', 'J', 27, '[', 'H', 0};
    for (unsigned i = 0; clear[i]; ++i)
        serial_char(clear[i]);
}
void knx_print_u64(uint64_t v) {
    char b[21];
    unsigned n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        knx_putc(b[--n]);
}
void knx_print_i64(int64_t v) {
    if (v < 0) {
        knx_putc('-');
        knx_print_u64((uint64_t)(-(v + 1)) + 1);
    } else
        knx_print_u64((uint64_t)v);
}
