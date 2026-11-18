#include "knx.h"
#include "sound.h"

#define KNX_SOUND_MAX_BACKENDS 4

static const knx_sound_backend *backends[KNX_SOUND_MAX_BACKENDS];
static unsigned backend_count;
static const knx_sound_backend *active_backend;

const knx_pcm_format knx_pcm_legacy_format = { 44100u, 1u, 16u };

int knx_sound_register(const knx_sound_backend *backend) {
    if (!backend || !backend->probe || !backend->start ||
        !backend->write || !backend->tick)
        return -22;
    for (unsigned i = 0; i < backend_count; ++i)
        if (backends[i] == backend)
            return 0;
    if (backend_count >= KNX_SOUND_MAX_BACKENDS)
        return -28;
    backends[backend_count++] = backend;
    if (!active_backend || backend->priority > active_backend->priority)
        active_backend = backend;
    return 1;
}

int knx_sound_start(void) {
    if (!active_backend)
        return -19;
    return active_backend->start();
}

int knx_sound_write(const uint8_t *buf, uint64_t len, const knx_pcm_format *fmt) {
    if (!active_backend)
        return -19;
    if (!fmt)
        return -22;
    return active_backend->write(buf, len, fmt);
}

void knx_sound_tick(void) {
    if (active_backend)
        active_backend->tick();
}

const char *knx_sound_active_name(void) {
    return active_backend ? active_backend->name : 0;
}

#define ES1370_CONTROL       0x00u
#define ES1370_SERIAL_CTRL   0x20u
#define ES1370_DAC1_SCOUNT   0x24u
#define ES1370_MEMPAGE       0x0cu
#define ES1370_DAC1_FRAMEADR 0x30u
#define ES1370_DAC1_FRAMECNT 0x34u
#define ES1370_PAGE_C        0x0cu

#define ES1370_CTRL_DAC1_EN  0x00000040u
#define ES1370_SCTRL_P1FMT   0x00000003u
#define ES1370_SCTRL_P1PAUSE 0x00000800u

#define ES1370_VENDOR_ID 0x1274u
#define ES1370_DEVICE_ID 0x5000u

#define ES1370_RATE          44100u
#define ES1370_RING_BYTES    (256u * 1024u)
#define ES1370_WIN_BYTES     (64u * 1024u)
#define ES1370_WIN_WORDS     (ES1370_WIN_BYTES / 4u)

static uint16_t es1370_io_base;
static int es1370_ready;
static uint8_t *es1370_ring;
static uint32_t es1370_ring_phys;
static uint64_t es1370_fill_pos;
static uint64_t es1370_window_off;
static uint16_t es1370_last_pos;

static void es1370_reg_out(uint16_t off, uint32_t value) {
    knx_io_out32((uint16_t)(es1370_io_base + off), value);
}

static uint32_t es1370_reg_in(uint16_t off) {
    return knx_io_in32((uint16_t)(es1370_io_base + off));
}

static void es1370_frame_out(uint16_t off, uint32_t value) {
    es1370_reg_out(ES1370_MEMPAGE, ES1370_PAGE_C);
    es1370_reg_out(off, value);
    es1370_reg_out(ES1370_MEMPAGE, 0);
}

static uint32_t es1370_frame_in(uint16_t off) {
    es1370_reg_out(ES1370_MEMPAGE, ES1370_PAGE_C);
    uint32_t value = es1370_reg_in(off);
    es1370_reg_out(ES1370_MEMPAGE, 0);
    return value;
}

static uint8_t *es1370_ring_alloc(uint64_t pages) {
    uint8_t *ring = (uint8_t *)knx_alloc_page();
    if (!ring)
        return 0;
    for (uint64_t p = 1; p < pages; ++p) {
        uint8_t *q = (uint8_t *)knx_alloc_page();
        if (!q || q != ring + p * 4096u) {
            for (uint64_t i = 0; i < p; ++i)
                knx_free_page(ring + i * 4096u);
            if (q)
                knx_free_page(q);
            return 0;
        }
    }
    for (uint64_t i = 0; i < pages * 4096u; ++i)
        ring[i] = 0;
    return ring;
}

static int es1370_hw_probe(const knx_pci_device *device) {
    if (!device || es1370_ready)
        return 0;
    if (device->vendor_id != ES1370_VENDOR_ID ||
        device->device_id != ES1370_DEVICE_ID)
        return 0;
    uint64_t bar = device->bar[0];
    if (!(bar & 1u))
        return 0;
    uint16_t base = (uint16_t)(bar & 0xfffffcu);
    if (!base)
        return 0;
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_IO) != 0) {
        knx_log("[audio]", "Could not enable the ES1370 I/O BAR");
        return 0;
    }
    uint8_t *ring = es1370_ring_alloc(ES1370_RING_BYTES / 4096u);
    if (!ring) {
        knx_log("[audio]", "ES1370 present but the 256 KiB ring would not fit");
        return 0;
    }
    es1370_io_base = base;
    es1370_ring = ring;
    es1370_ring_phys = (uint32_t)(uintptr_t)ring;

    es1370_reg_out(ES1370_SERIAL_CTRL, ES1370_SCTRL_P1PAUSE);
    es1370_reg_out(ES1370_DAC1_SCOUNT, 0x01ff01ffu);
    es1370_frame_out(ES1370_DAC1_FRAMEADR, es1370_ring_phys);
    es1370_frame_out(ES1370_DAC1_FRAMECNT, (uint32_t)(ES1370_WIN_WORDS - 1u));
    es1370_reg_out(ES1370_CONTROL,
                   ES1370_CTRL_DAC1_EN | (3u << 12));
    es1370_reg_out(ES1370_SERIAL_CTRL, 2u);

    es1370_ready = 1;
    knx_log("[audio]", "ES1370 DAC1: ok (44100 mono s16, 256 KiB ring, "
                       "64 KiB windows, tick-polled)");
    return 1;
}

static int es1370_start(void) {
    if (!es1370_ready)
        return -19;

    return 0;
}

static void es1370_tick(void) {
    if (!es1370_ready)
        return;
    uint32_t fc = es1370_frame_in(ES1370_DAC1_FRAMECNT);
    uint16_t pos = (uint16_t)(fc >> 16);
    if (pos < es1370_last_pos) {

        es1370_reg_out(ES1370_SERIAL_CTRL,
                       (uint32_t)(es1370_reg_in(ES1370_SERIAL_CTRL) |
                                  ES1370_SCTRL_P1PAUSE));
        es1370_window_off =
            (es1370_window_off + ES1370_WIN_BYTES) % ES1370_RING_BYTES;
        es1370_frame_out(ES1370_DAC1_FRAMEADR,
                         es1370_ring_phys + (uint32_t)es1370_window_off);
        es1370_frame_out(ES1370_DAC1_FRAMECNT,
                         (uint32_t)(ES1370_WIN_WORDS - 1u));
        es1370_reg_out(ES1370_DAC1_SCOUNT, 0x01ff01ffu);
        es1370_reg_out(ES1370_SERIAL_CTRL,
                       (uint32_t)(es1370_reg_in(ES1370_SERIAL_CTRL) &
                                  ~ES1370_SCTRL_P1PAUSE));
        es1370_last_pos = 0;
    } else {
        es1370_last_pos = pos;
    }
}

static int es1370_write(const uint8_t *buf, uint64_t len,
                        const knx_pcm_format *fmt) {
    if (!es1370_ready)
        return -19;
    if (!fmt || fmt->bits != 16u || fmt->channels != 1u)
        return -22;
    if (!len)
        return 0;
    uint64_t limit =
        (es1370_window_off + ES1370_WIN_BYTES) % ES1370_RING_BYTES;
    uint64_t room =
        (limit - es1370_fill_pos + ES1370_RING_BYTES) % ES1370_RING_BYTES;
    if (len > room)
        len = room;
    if (!len)
        return 0;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t first = ES1370_RING_BYTES - es1370_fill_pos;
    if (first > len)
        first = len;
    for (uint64_t i = 0; i < first; ++i)
        es1370_ring[es1370_fill_pos + i] = buf[i];
    for (uint64_t i = first; i < len; ++i)
        es1370_ring[i - first] = buf[i];
    es1370_fill_pos = (es1370_fill_pos + len) % ES1370_RING_BYTES;
    __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
    return (int)len;
}

static const knx_sound_backend es1370_backend = {
    .name = "ES1370 DAC1 (mono s16 44.1kHz)",
    .priority = 1,
    .probe = es1370_hw_probe,
    .start = es1370_start,
    .write = es1370_write,
    .tick = es1370_tick,
};

int knx_es1370_probe(const knx_pci_device *device) {
    if (!es1370_hw_probe(device))
        return 0;
    knx_sound_register(&es1370_backend);
    return 1;
}

void knx_audio_tick(void) {
    knx_sound_tick();
}

int knx_pcm_write(const uint8_t *buf, uint64_t len) {
    return knx_sound_write(buf, len, &knx_pcm_legacy_format);
}
