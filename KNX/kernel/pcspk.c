#include "knx.h"

#define PIT_CMD_PORT 0x43
#define PIT_CH2_PORT 0x42
#define PIT_BASE_HZ 1193182u
#define SPK_PORT 0x61

static uint64_t beep_until;
static uint8_t speaker_on;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

void knx_pcspk_init(void) {
    outb(SPK_PORT, (uint8_t)(inb(SPK_PORT) & ~0x03u));
    speaker_on = 0;
}

void knx_pcspk_beep(uint32_t hz, uint32_t ms) {
    if (!hz)
        hz = 1000;
    if (hz > 20000)
        hz = 20000;
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor < 2)
        divisor = 2;
    outb(PIT_CMD_PORT, 0xb6);
    outb(PIT_CH2_PORT, (uint8_t)(divisor & 0xff));
    outb(PIT_CH2_PORT, (uint8_t)(divisor >> 8));
    outb(SPK_PORT, (uint8_t)(inb(SPK_PORT) | 0x03u));

    beep_until = knx_ticks() + (ms + 9) / 10;
    speaker_on = 1;
}

void knx_pcspk_tick(void) {
    if (!speaker_on || knx_ticks() < beep_until)
        return;
    outb(SPK_PORT, (uint8_t)(inb(SPK_PORT) & ~0x03u));
    speaker_on = 0;
}
