#pragma once
#include "knx.h"

typedef struct {
    uint32_t rate;
    uint8_t channels;
    uint8_t bits;
} knx_pcm_format;

typedef struct knx_sound_backend {
    const char *name;
    int priority;
    int (*probe)(const knx_pci_device *device);
    int (*start)(void);
    int (*write)(const uint8_t *buf, uint64_t len, const knx_pcm_format *fmt);
    void (*tick)(void);
} knx_sound_backend;

int knx_sound_register(const knx_sound_backend *backend);
int knx_sound_start(void);
int knx_sound_write(const uint8_t *buf, uint64_t len, const knx_pcm_format *fmt);
void knx_sound_tick(void);
const char *knx_sound_active_name(void);
extern const knx_pcm_format knx_pcm_legacy_format;
