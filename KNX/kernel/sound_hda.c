#include "knx.h"
#include "sound.h"

#define HDA_GCAP        0x00u
#define HDA_VMIN        0x02u
#define HDA_VMAJ        0x03u
#define HDA_GCTL        0x08u
#define HDA_STATESTS    0x0eu
#define HDA_GSTS        0x10u
#define HDA_INTCTL      0x20u
#define HDA_INTSTS      0x24u
#define HDA_CORBLBASE   0x40u
#define HDA_CORBUBASE   0x44u
#define HDA_CORBWP      0x48u
#define HDA_CORBRP      0x4au
#define HDA_CORBCTL     0x4cu
#define HDA_CORBSTS     0x4du
#define HDA_CORBSIZE    0x4eu
#define HDA_RIRBLBASE   0x50u
#define HDA_RIRBUBASE   0x54u
#define HDA_RIRBWP      0x58u
#define HDA_RINTCNT     0x5au
#define HDA_RIRBCTL     0x5cu
#define HDA_RIRBSTS     0x5du
#define HDA_RIRBSIZE    0x5eu

#define HDA_SD0_BASE    0x100u
#define HDA_SD_CTL      0x00u
#define HDA_SD_STS      0x03u
#define HDA_SD_LPIB     0x04u
#define HDA_SD_CBL      0x08u
#define HDA_SD_LVI      0x0cu
#define HDA_SD_FMT      0x12u
#define HDA_SD_BDLPL    0x18u
#define HDA_SD_BDLPU    0x1cu

#define HDA_GCTL_RESET   0x01u
#define HDA_GCTL_FCNTRL  0x02u
#define HDA_GSTS_FSTS    0x02u
#define HDA_CORBRP_RST   0x8000u
#define HDA_RIRBWP_RST   0x8000u
#define HDA_CORBCTL_RUN  0x02u
#define HDA_RIRBCTL_DMA  0x02u
#define HDA_SDCTL_RESET  0x01u
#define HDA_SDCTL_RUN    0x02u
#define HDA_TAG_SHIFT    20u

#define V_GET_PARAMETER      0xf00u
#define V_SET_STREAM_FORMAT  0x200u
#define V_SET_AMP_GAIN_MUTE  0x300u
#define V_SET_POWER_STATE    0x705u
#define V_SET_CHANNEL_STREAM 0x706u
#define V_SET_PIN_WIDGET     0x707u

#define P_NODE_COUNT 0x04u
#define P_FUNC_TYPE  0x05u
#define P_WIDGET_CAP 0x09u
#define P_PIN_CAP    0x0cu

#define WID_AUDIO_OUT 0x0u
#define WID_PIN       0x4u
#define PINCAP_OUT    0x10u
#define PINCTL_OUT_EN 0x40u

#define HDA_FORMAT_44100_STEREO16 0x4011u
#define HDA_STREAM_TAG 1u

#define HDA_MMIO_SIZE      0x2000u
#define HDA_BDL_ENTRIES    16u
#define HDA_PAGE_BYTES     4096u
#define HDA_BUF_BYTES      (HDA_BDL_ENTRIES * HDA_PAGE_BYTES)

#define HDA_CMD(cad, nid, verb, data)                                   \
    (((uint32_t)(cad) << 28) | ((uint32_t)(nid) << 20) |                \
     ((uint32_t)(verb) << 8) | (uint16_t)(data))

typedef struct {
    volatile uint8_t *mmio;
    volatile uint32_t *corb;
    volatile uint32_t *rirb;
    uint8_t corb_wp;
    uint8_t rirb_wp;
    uint8_t cad;
    uint8_t dac_nid;
    uint8_t pin_nid;
    uint8_t ready;
    uint8_t *pages[HDA_BDL_ENTRIES];
    volatile uint32_t *bdl;
    uint32_t bdl_phys;
    uint64_t fill;
    uint64_t consumed;
    uint32_t last_lpib;
    uint64_t wraps;
} hda_dev;

static hda_dev hda;

static inline uint32_t hda_rd32(uint32_t off) {
    return *(volatile uint32_t *)(hda.mmio + off);
}
static inline uint16_t hda_rd16(uint32_t off) {
    return *(volatile uint16_t *)(hda.mmio + off);
}
static inline uint8_t hda_rd8(uint32_t off) {
    return *(volatile uint8_t *)(hda.mmio + off);
}
static inline void hda_wr32(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(hda.mmio + off) = value;
}
static inline void hda_wr16(uint32_t off, uint16_t value) {
    *(volatile uint16_t *)(hda.mmio + off) = value;
}
static inline void hda_wr8(uint32_t off, uint8_t value) {
    *(volatile uint8_t *)(hda.mmio + off) = value;
}

static void hda_delay(unsigned loops) {
    while (loops--)
        __asm__ volatile("pause");
}

static void hda_release(void) {
    if (hda.bdl) {
        knx_free_page((void *)(uintptr_t)hda.bdl);
        hda.bdl = 0;
    }
    for (unsigned i = 0; i < HDA_BDL_ENTRIES; ++i) {
        if (hda.pages[i]) {
            knx_free_page(hda.pages[i]);
            hda.pages[i] = 0;
        }
    }
    if (hda.corb) {
        knx_free_page((void *)(uintptr_t)hda.corb);
        hda.corb = 0;
    }
    if (hda.rirb) {
        knx_free_page((void *)(uintptr_t)hda.rirb);
        hda.rirb = 0;
    }
    hda.mmio = 0;
    hda.ready = 0;
}

static uint32_t hda_codec_cmd(uint8_t nid, uint16_t verb, uint16_t data) {
    uint32_t command = HDA_CMD(hda.cad, nid, verb, data);
    uint8_t wp = (uint8_t)((hda.corb_wp + 1u) & 0xffu);
    hda.corb[wp] = command;
    hda.corb_wp = wp;
    hda_wr16(HDA_CORBWP, wp);

    uint8_t before = hda.rirb_wp;
    uint8_t now = before;
    unsigned timeout = 200000u;
    while (timeout--) {
        now = (uint8_t)(hda_rd16(HDA_RIRBWP) & 0xffu);
        if (now != before)
            break;
        hda_delay(4);
    }
    if (now == before)
        return 0;
    hda.rirb_wp = now;
    return hda.rirb[(uint32_t)now * 2u];
}

static unsigned hda_node_count(uint32_t value, unsigned *start) {
    *start = (unsigned)((value >> 8) & 0xffu);
    return (unsigned)(value & 0xffu);
}

static int hda_hw_probe(const knx_pci_device *device) {
    if (!device || hda.ready)
        return 0;
    if (device->class_code != 0x04u || device->subclass != 0x03u)
        return 0;
    uint64_t bar = knx_pci_mmio_bar(device, 0);
    if (!bar) {
        knx_log("[HDA]", "No MMIO BAR, rejecting controller");
        return 0;
    }
    if (knx_vm_map_mmio(bar, HDA_MMIO_SIZE) != 0) {
        knx_log("[HDA]", "Cannot map the MMIO BAR");
        return 0;
    }
    if (knx_pci_enable_device(device,
                              KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER) != 0) {
        knx_log("[HDA]", "Cannot enable the PCI function");
        return 0;
    }
    hda.mmio = (volatile uint8_t *)(uintptr_t)bar;

    unsigned gcap = hda_rd32(HDA_GCAP);
    unsigned out_streams = (gcap >> 12) & 0xfu;
    uint8_t vmaj = hda_rd8(HDA_VMAJ);
    if (!vmaj || !out_streams) {
        knx_log("[HDA]", "Unusable controller (version/stream count)");
        goto fail;
    }

    hda_wr32(HDA_INTCTL, 0);
    hda_wr32(HDA_GCTL, HDA_GCTL_RESET);
    hda_delay(2000);
    hda_wr32(HDA_GCTL, HDA_GCTL_FCNTRL);
    for (unsigned i = 0; i < 10000u && (hda_rd32(HDA_GSTS) & HDA_GSTS_FSTS); ++i)
        hda_delay(10);
    hda_wr32(HDA_GCTL, 0);
    hda_delay(200);

    {
        uint16_t state = hda_rd16(HDA_STATESTS);
        unsigned cad = 0;
        if (state) {
            while (!(state & 1u) && cad < 15u) {
                state = (uint16_t)(state >> 1);
                ++cad;
            }
        }
        hda.cad = (uint8_t)cad;
    }

    hda.corb = (volatile uint32_t *)knx_alloc_page();
    hda.rirb = (volatile uint32_t *)knx_alloc_page();
    if (!hda.corb || !hda.rirb) {
        knx_log("[HDA]", "Cannot allocate CORB/RIRB rings");
        goto fail;
    }
    for (unsigned i = 0; i < 256u; ++i) {
        hda.corb[i] = 0;
        hda.rirb[i * 2u] = 0;
        hda.rirb[i * 2u + 1u] = 0;
    }
    hda_wr16(HDA_CORBRP, HDA_CORBRP_RST);
    hda_delay(100);
    hda_wr16(HDA_CORBRP, 0);
    hda_wr16(HDA_RIRBWP, HDA_RIRBWP_RST);
    hda_delay(100);
    hda.corb_wp = 0;
    hda.rirb_wp = 0;
    {
        uint32_t corb_phys = (uint32_t)(uintptr_t)hda.corb;
        uint32_t rirb_phys = (uint32_t)(uintptr_t)hda.rirb;
        hda_wr32(HDA_CORBLBASE, corb_phys & 0xffffff80u);
        hda_wr32(HDA_CORBUBASE, 0);
        hda_wr32(HDA_RIRBLBASE, rirb_phys & 0xfffffe00u);
        hda_wr32(HDA_RIRBUBASE, 0);
    }
    hda_wr16(HDA_RINTCNT, 0xffu);
    hda_wr8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_wr8(HDA_RIRBCTL, HDA_RIRBCTL_DMA);

    {
        unsigned start, count;
        uint32_t r = hda_codec_cmd(0, V_GET_PARAMETER, P_NODE_COUNT);
        count = hda_node_count(r, &start);
        uint8_t afg = 0;
        for (unsigned n = start; n < start + count && n < 256u; ++n) {
            uint32_t ft = hda_codec_cmd((uint8_t)n, V_GET_PARAMETER, P_FUNC_TYPE);
            if ((ft & 0xffu) == 1u) {
                afg = (uint8_t)n;
                break;
            }
        }
        if (!afg) {
            knx_log("[HDA]", "No audio function group found");
            goto fail;
        }
        r = hda_codec_cmd(afg, V_GET_PARAMETER, P_NODE_COUNT);
        count = hda_node_count(r, &start);
        for (unsigned n = start; n < start + count && n < 256u; ++n) {
            uint32_t cap = hda_codec_cmd((uint8_t)n, V_GET_PARAMETER,
                                         P_WIDGET_CAP);
            unsigned type = (unsigned)((cap >> 20) & 0xfu);
            if (type == WID_AUDIO_OUT && (cap & 1u) && !hda.dac_nid)
                hda.dac_nid = (uint8_t)n;
            if (type == WID_PIN && !hda.pin_nid) {
                uint32_t pcap = hda_codec_cmd((uint8_t)n, V_GET_PARAMETER,
                                              P_PIN_CAP);
                if (pcap & PINCAP_OUT)
                    hda.pin_nid = (uint8_t)n;
            }
        }
        if (!hda.dac_nid) {
            knx_log("[HDA]", "No output converter widget found");
            goto fail;
        }

        hda_codec_cmd(afg, V_SET_POWER_STATE, 0);
        hda_codec_cmd(hda.dac_nid, V_SET_POWER_STATE, 0);
        hda_codec_cmd(hda.dac_nid, V_SET_STREAM_FORMAT,
                      HDA_FORMAT_44100_STEREO16);
        hda_codec_cmd(hda.dac_nid, V_SET_CHANNEL_STREAM,
                      (uint16_t)((HDA_STREAM_TAG << 4) | 0u));
        hda_codec_cmd(hda.dac_nid, V_SET_AMP_GAIN_MUTE, 0xb07fu);
        if (hda.pin_nid) {
            hda_codec_cmd(hda.pin_nid, V_SET_POWER_STATE, 0);
            hda_codec_cmd(hda.pin_nid, V_SET_PIN_WIDGET, PINCTL_OUT_EN);
        }
    }

    hda.bdl = (volatile uint32_t *)knx_alloc_page();
    if (!hda.bdl) {
        knx_log("[HDA]", "Cannot allocate the BDL page");
        goto fail;
    }
    for (unsigned i = 0; i < HDA_BDL_ENTRIES * 4u; ++i)
        hda.bdl[i] = 0;
    hda.bdl_phys = (uint32_t)(uintptr_t)hda.bdl;
    for (unsigned i = 0; i < HDA_BDL_ENTRIES; ++i) {
        uint8_t *p = (uint8_t *)knx_alloc_page();
        if (!p) {
            knx_log("[HDA]", "Cannot allocate a DMA buffer page");
            goto fail;
        }
        hda.pages[i] = p;
        for (unsigned j = 0; j < HDA_PAGE_BYTES; ++j)
            p[j] = 0;
        {
            uint32_t phys = (uint32_t)(uintptr_t)p;
            hda.bdl[i * 4u] = phys;
            hda.bdl[i * 4u + 1u] = 0;
            hda.bdl[i * 4u + 2u] = HDA_PAGE_BYTES;
            hda.bdl[i * 4u + 3u] = 0;
        }
    }

    hda_wr32(HDA_SD0_BASE + HDA_SD_CTL, HDA_SDCTL_RESET);
    hda_delay(500);
    hda_wr16(HDA_SD0_BASE + HDA_SD_FMT, HDA_FORMAT_44100_STEREO16);
    hda_wr16(HDA_SD0_BASE + HDA_SD_LVI, (uint16_t)(HDA_BDL_ENTRIES - 1u));
    hda_wr32(HDA_SD0_BASE + HDA_SD_CBL, HDA_BUF_BYTES);
    hda_wr32(HDA_SD0_BASE + HDA_SD_BDLPL, hda.bdl_phys & 0xffffff80u);
    hda_wr32(HDA_SD0_BASE + HDA_SD_BDLPU, 0);
    hda_wr32(HDA_SD0_BASE + HDA_SD_CTL,
             (HDA_STREAM_TAG << HDA_TAG_SHIFT) | HDA_SDCTL_RUN);

    hda.fill = 0;
    hda.consumed = 0;
    hda.last_lpib = 0;
    hda.wraps = 0;
    hda.ready = 1;
    knx_log("[HDA]", "Ok: 44100 stereo s16, 64 KiB BDL, codec discovered");
    return 1;

fail:
    hda_release();
    return 0;
}

static int hda_start(void) {
    if (!hda.ready)
        return -19;

    hda_wr32(HDA_SD0_BASE + HDA_SD_CTL,
             (HDA_STREAM_TAG << HDA_TAG_SHIFT) | HDA_SDCTL_RUN);
    return 0;
}

static void hda_refresh_pos(void) {
    if (!hda.ready)
        return;
    uint32_t lpib = hda_rd32(HDA_SD0_BASE + HDA_SD_LPIB);
    if (lpib >= HDA_BUF_BYTES)
        lpib = 0;
    if (lpib < hda.last_lpib)
        ++hda.wraps;
    hda.last_lpib = lpib;
    hda.consumed = hda.wraps * HDA_BUF_BYTES + lpib;

    if (hda.fill < hda.consumed)
        hda.fill = hda.consumed;
}

static void hda_tick(void) {
    hda_refresh_pos();
}

static void hda_ring_put(uint8_t frame[4]) {
    uint64_t off = hda.fill % HDA_BUF_BYTES;
    unsigned page = (unsigned)(off / HDA_PAGE_BYTES);
    unsigned in = (unsigned)(off % HDA_PAGE_BYTES);
    for (unsigned i = 0; i < 4u; ++i) {
        if (in + i < HDA_PAGE_BYTES)
            hda.pages[page][in + i] = frame[i];
        else
            hda.pages[(page + 1u) % HDA_BDL_ENTRIES][in + i - HDA_PAGE_BYTES] =
                frame[i];
    }
    hda.fill += 4u;
}

static int hda_write(const uint8_t *buf, uint64_t len,
                     const knx_pcm_format *fmt) {
    if (!hda.ready)
        return -19;
    if (!fmt || fmt->bits != 16u ||
        (fmt->channels != 1u && fmt->channels != 2u))
        return -22;

    unsigned src_frame = (unsigned)fmt->channels * 2u;
    hda_refresh_pos();
    uint64_t ahead = hda.fill - hda.consumed;
    if (ahead > HDA_BUF_BYTES)
        return 0;
    uint64_t room = HDA_BUF_BYTES - ahead;
    uint64_t src_frames = len / src_frame;
    uint64_t max_frames = room / 4u;
    if (src_frames > max_frames)
        src_frames = max_frames;
    if (!src_frames)
        return 0;
    uint64_t accepted = src_frames * src_frame;

    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile("cli");
    for (uint64_t f = 0; f < src_frames; ++f) {
        uint8_t frame[4];
        if (fmt->channels == 1u) {
            frame[0] = buf[f * 2u];
            frame[1] = buf[f * 2u + 1u];
            frame[2] = frame[0];
            frame[3] = frame[1];
        } else {
            frame[0] = buf[f * 4u];
            frame[1] = buf[f * 4u + 1u];
            frame[2] = buf[f * 4u + 2u];
            frame[3] = buf[f * 4u + 3u];
        }
        hda_ring_put(frame);
    }
    __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
    return (int)accepted;
}

static const knx_sound_backend hda_backend = {
    .name = "HDA Azalia (stereo s16 44.1kHz)",
    .priority = 2,
    .probe = hda_hw_probe,
    .start = hda_start,
    .write = hda_write,
    .tick = hda_tick,
};

int knx_hda_probe(const knx_pci_device *device) {
    if (!hda_hw_probe(device))
        return 0;
    knx_sound_register(&hda_backend);
    return 1;
}
