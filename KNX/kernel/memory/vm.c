#include "knx.h"
#define PAGE_ENTRIES 512
static uint64_t pml4[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pdpt[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t page_directories[64][PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t low_page_table[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t user_stack_table[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_root;
#define VM_USER_DYNAMIC_FIRST 256u
#define VM_USER_DYNAMIC_LAST 512u
#define VM_PAGE_SIZE 4096ULL

#define KNX_PTE_COW 0x200ULL
#define KNX_PTE_OWNED 0x400ULL
extern char __user_text_start, __user_text_end, __user_stack_start, __user_stack_end;

static void setup_pat(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    uint64_t pat = ((uint64_t)hi << 32) | lo;
    pat &= ~(0xfull << 24);
    pat |= (0x1ull << 24);
    __asm__ volatile("wrmsr" :: "a"((uint32_t)pat), "d"((uint32_t)(pat >> 32)),
                     "c"(0x277) : "memory");
}

void knx_vm_init(void) {
    setup_pat();
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        pml4[i] = 0;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        pdpt[i] = 0;
    for (unsigned d = 0; d < 64; ++d) {
        for (unsigned p = 0; p < PAGE_ENTRIES; ++p)
            page_directories[d][p] = (((uint64_t)d * PAGE_ENTRIES + p) * 0x200000ULL) | 0x83;
        pdpt[d] = (uint64_t)(uintptr_t)page_directories[d] | 3;
    }

    for (unsigned p = 0; p < PAGE_ENTRIES; ++p)
        low_page_table[p] = ((uint64_t)p * 4096) | 3;
    for (uint64_t a = (uint64_t)(uintptr_t)&__user_text_start;
         a < (uint64_t)(uintptr_t)&__user_text_end; a += 4096)
        low_page_table[a / 4096] = (low_page_table[a / 4096] & ~2ULL) | 5ULL;
    page_directories[0][0] = (uint64_t)(uintptr_t)low_page_table | 7;

    uint64_t stack_phys = (uint64_t)(uintptr_t)&__user_stack_start;
    uint64_t stack_end = (uint64_t)(uintptr_t)&__user_stack_end;
    for (uint64_t a = stack_phys, i = 0; a < stack_end; a += 4096, ++i)
        user_stack_table[i] = a | 7;
    page_directories[0][3] = (uint64_t)(uintptr_t)user_stack_table | 7;
    pdpt[0] |= 4;
    pml4[0] = (uint64_t)(uintptr_t)pdpt | 3;
    pml4[0] |= 4;
    uint64_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= 0x630;
    __asm__ volatile("mov %0,%%cr4" : : "r"(cr4) : "memory");
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 = (cr0 | (1u << 1) | 0x80000000u) & ~(1u << 2);
    __asm__ volatile("mov %0,%%cr0" : : "r"(cr0) : "memory");
    kernel_root = (uint64_t)(uintptr_t)pml4;
    __asm__ volatile("mov %0,%%cr3" : : "r"(kernel_root) : "memory");
}

static uint64_t *new_table(void) {
    uint64_t *p = knx_alloc_page();
    if (!p)
        return 0;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        p[i] = 0;
    return p;
}

#define VM_MMIO_TABLES 8
static uint64_t *mmio_tables[VM_MMIO_TABLES];
static unsigned mmio_table_count;

static uint64_t early_mmio_tables[4][PAGE_ENTRIES] __attribute__((aligned(4096)));
static unsigned early_mmio_count;

static uint64_t *mmio_table(void) {
    if (mmio_table_count >= VM_MMIO_TABLES)
        return 0;
    uint64_t *table;
    if (!knx_total_memory()) {
        if (early_mmio_count >= 4)
            return 0;
        table = early_mmio_tables[early_mmio_count++];
    } else {
        table = new_table();
    }
    if (!table)
        return 0;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        table[i] = 0;
    mmio_tables[mmio_table_count++] = table;
    return table;
}

int knx_vm_map_mmio(uint64_t physical, uint64_t size) {
    if (!size || physical + size < physical)
        return -1;
    uint64_t begin = physical & ~0x1fffffULL;
    uint64_t end = (physical + size + 0x1fffffULL) & ~0x1fffffULL;
    if (end <= begin)
        return -1;
    if (end <= (64ULL << 30)) {

        unsigned pml4_index = 0, pdpt_index = (unsigned)((begin >> 30) & 0x1ff);
        unsigned pd_index = (unsigned)((begin >> 21) & 0x1ff);
        uint64_t *l3 = (uint64_t *)(uintptr_t)(pml4[pml4_index] & ~0xfffULL);
        uint64_t *l2 = (uint64_t *)(uintptr_t)(l3[pdpt_index] & ~0xfffULL);
        for (uint64_t a = begin; a < end; a += 0x200000ULL) {
            pd_index = (unsigned)((a >> 21) & 0x1ff);
            l2[pd_index] = a | 0x93;
        }
        uint64_t root;
        __asm__ volatile("mov %%cr3,%0" : "=r"(root));
        __asm__ volatile("mov %0,%%cr3" : : "r"(root) : "memory");
        return 0;
    }
    for (uint64_t a = begin; a < end; a += 0x200000ULL) {
        unsigned pml4_index = (unsigned)((a >> 39) & 0x1ff);
        unsigned pdpt_index = (unsigned)((a >> 30) & 0x1ff);
        unsigned pd_index = (unsigned)((a >> 21) & 0x1ff);
        uint64_t *l3, *l2;
        if (!(pml4[pml4_index] & 1)) {
            l3 = mmio_table();
            if (!l3)
                return -1;
            pml4[pml4_index] = (uint64_t)(uintptr_t)l3 | 3;
        } else {
            l3 = (uint64_t *)(uintptr_t)(pml4[pml4_index] & ~0xfffULL);
        }
        if (!(l3[pdpt_index] & 1)) {
            l2 = mmio_table();
            if (!l2)
                return -1;
            l3[pdpt_index] = (uint64_t)(uintptr_t)l2 | 3;
        } else {
            l2 = (uint64_t *)(uintptr_t)(l3[pdpt_index] & ~0xfffULL);
        }

        if (!(l2[pd_index] & 1))
            l2[pd_index] = a | 0x93;
    }
    uint64_t root;
    __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    __asm__ volatile("mov %0,%%cr3" : : "r"(root) : "memory");
    return 0;
}

void *knx_vm_clone_user(void) {
    uint64_t *root = 0, *l3 = 0, *l2 = 0, *text_pt = 0, *exec_pt = 0, *stack_pt = 0;
    root = new_table();
    l3 = new_table();
    l2 = new_table();
    text_pt = new_table();
    stack_pt = new_table();
    if (!root || !l3 || !l2 || !text_pt || !stack_pt)
        goto fail;

    uint64_t current_root;
    __asm__ volatile("mov %%cr3,%0" : "=r"(current_root));
    uint64_t *source_root = (uint64_t *)(uintptr_t)(current_root & ~0xfffULL);
    uint64_t *source_l3 = (uint64_t *)(uintptr_t)(source_root[0] & ~0xfffULL);
    uint64_t *source_l2 = (uint64_t *)(uintptr_t)(source_l3[0] & ~0xfffULL);
    uint64_t *source_text_pt = (uint64_t *)(uintptr_t)(source_l2[0] & ~0xfffULL);
    uint64_t *source_stack_pt = (uint64_t *)(uintptr_t)(source_l2[3] & ~0xfffULL);
    uint64_t *source_exec_pt = 0;
    if (!(source_l2[2] & 0x80)) {
        source_exec_pt = (uint64_t *)(uintptr_t)(source_l2[2] & ~0xfffULL);
        exec_pt = new_table();
        if (!exec_pt)
            goto fail;
    }
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        root[i] = source_root[i];
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l3[i] = source_l3[i];
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l2[i] = source_l2[i];
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
        text_pt[i] = source_text_pt[i];
        if (((text_pt[i] & 7) == 7 && (text_pt[i] & 2)) ||
            (text_pt[i] & KNX_PTE_OWNED)) {
            uint64_t flags = (text_pt[i] & 0xfffULL) & ~KNX_PTE_COW;
            flags |= KNX_PTE_OWNED;
            if (text_pt[i] & KNX_PTE_COW)
                flags |= 2ULL;
            void *page = knx_alloc_page();
            if (!page)
                goto fail;
            uint8_t *dst = page;
            uint8_t *src = (uint8_t *)(uintptr_t)(text_pt[i] & ~0xfffULL);
            for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
                dst[b] = src[b];
            text_pt[i] = (uint64_t)(uintptr_t)page | flags;
        }
        stack_pt[i] = 0;
    }

    if (source_exec_pt) {
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t entry = source_exec_pt[i];
            if (!(entry & 1))
                continue;
            void *page = knx_alloc_page();
            if (!page)
                goto fail;
            uint8_t *dst = page;
            uint8_t *src = (uint8_t *)(uintptr_t)(entry & ~0xfffULL);
            for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
                dst[b] = src[b];
            uint64_t flags = entry & 0xfffULL;
            if (flags & KNX_PTE_COW)
                flags = (flags & ~KNX_PTE_COW) | 2ULL;
            exec_pt[i] = (uint64_t)(uintptr_t)page | flags | KNX_PTE_OWNED;
        }
    }

    for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
        uint64_t source_entry = source_stack_pt[i];
        if (!(source_entry & 1))
            continue;
        uint64_t *page = knx_alloc_page();
        if (!page)
            goto fail;
        uint8_t *dst = (uint8_t *)page;
        uint8_t *src = (uint8_t *)(uintptr_t)(source_entry & ~0xfffULL);
        for (unsigned b = 0; b < 4096; ++b)
            dst[b] = src[b];
        stack_pt[i] = ((uint64_t)(uintptr_t)page) | (source_entry & 0xfffULL);
    }

    l2[0] = (uint64_t)(uintptr_t)text_pt | 7;
    if (exec_pt)
        l2[2] = (uint64_t)(uintptr_t)exec_pt | 7;
    l2[3] = (uint64_t)(uintptr_t)stack_pt | 7;
    l3[0] = (uint64_t)(uintptr_t)l2 | 7;
    root[0] = (uint64_t)(uintptr_t)l3 | 7;
    return root;

fail:
    if (exec_pt)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (exec_pt[i] & 1)
                knx_free_page((void *)(uintptr_t)(exec_pt[i] & ~0xfffULL));
    if (text_pt)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if ((text_pt[i] & (1ULL | KNX_PTE_OWNED)) == (1ULL | KNX_PTE_OWNED))
                knx_free_page((void *)(uintptr_t)(text_pt[i] & ~0xfffULL));
    if (stack_pt)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (stack_pt[i] & 1)
                knx_free_page((void *)(uintptr_t)(stack_pt[i] & ~0xfffULL));
    if (stack_pt)
        knx_free_page(stack_pt);
    if (exec_pt)
        knx_free_page(exec_pt);
    if (text_pt)
        knx_free_page(text_pt);
    if (l2)
        knx_free_page(l2);
    if (l3)
        knx_free_page(l3);
    if (root)
        knx_free_page(root);
    return 0;
}

void knx_vm_destroy_user(void *address_space) {
    if (!address_space || (uint64_t)(uintptr_t)address_space == kernel_root)
        return;
    uint64_t *root = address_space;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(root[0] & ~0xfffULL);
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l3[0] & ~0xfffULL);
    uint64_t *text_pt = (uint64_t *)(uintptr_t)(l2[0] & ~0xfffULL);
    int elf_space = !(l2[2] & 0x80);
    uint64_t *exec_pt = elf_space ? (uint64_t *)(uintptr_t)(l2[2] & ~0xfffULL) : 0;
    uint64_t *stack_pt = (uint64_t *)(uintptr_t)(l2[3] & ~0xfffULL);
    if (exec_pt)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (exec_pt[i] & 1)
                knx_free_page((void *)(uintptr_t)(exec_pt[i] & 0x000ffffffffff000ULL));
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        if ((text_pt[i] & (1ULL | KNX_PTE_OWNED)) == (1ULL | KNX_PTE_OWNED))
            knx_free_page((void *)(uintptr_t)(text_pt[i] & ~0xfffULL));

    uint64_t stack_lo = (uint64_t)(uintptr_t)&__user_stack_start;
    uint64_t stack_hi = (uint64_t)(uintptr_t)&__user_stack_end;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        if (stack_pt[i] & 1) {
            uint64_t frame = stack_pt[i] & 0x000ffffffffff000ULL;
            if (frame < stack_lo || frame >= stack_hi)
                knx_free_page((void *)(uintptr_t)frame);
        }
    knx_free_page(stack_pt);
    if (exec_pt)
        knx_free_page(exec_pt);
    if (text_pt != low_page_table)
        knx_free_page(text_pt);
    knx_free_page(l2);
    knx_free_page(l3);
    knx_free_page(root);
}

void knx_vm_switch(void *address_space) {
    uint64_t root = address_space ? (uint64_t)(uintptr_t)address_space : kernel_root;
    __asm__ volatile("mov %0,%%cr3" : : "r"(root) : "memory");
}

static uint64_t *active_user_pt(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    uint64_t *root = (uint64_t *)(uintptr_t)(cr3 & ~0xfffULL);
    if (!(root[0] & 1))
        return 0;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(root[0] & ~0xfffULL);
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l3[0] & ~0xfffULL);
    if (!(l2[3] & 1))
        return 0;
    return (uint64_t *)(uintptr_t)(l2[3] & ~0xfffULL);
}

int64_t knx_vm_map_anonymous(uint64_t size, uint64_t permissions) {
    if (!size || size > (VM_USER_DYNAMIC_LAST - VM_USER_DYNAMIC_FIRST) * VM_PAGE_SIZE ||
        !(permissions & 1) || (permissions & ~7ULL) ||
        ((permissions & 2) && (permissions & 4)))
        return -22;
    uint64_t pages = (size + VM_PAGE_SIZE - 1) / VM_PAGE_SIZE;
    uint64_t *pt = active_user_pt();
    if (!pt)
        return -12;
    unsigned run = 0, start = VM_USER_DYNAMIC_FIRST;
    for (unsigned i = VM_USER_DYNAMIC_FIRST; i < VM_USER_DYNAMIC_LAST; ++i) {
        if (!(pt[i] & 1)) {
            if (!run)
                start = i;
            if (++run == pages)
                goto found;
        } else
            run = 0;
    }
    return -12;
found:
    for (unsigned n = 0; n < pages; ++n) {
        void *page = knx_alloc_page();
        if (!page) {
            while (n) {
                --n;
                knx_free_page((void *)(uintptr_t)(pt[start + n] & ~0xfffULL));
                pt[start + n] = 0;
            }
            return -12;
        }
        uint8_t *bytes = page;
        for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
            bytes[b] = 0;
        pt[start + n] = (uint64_t)(uintptr_t)page | 5 | ((permissions & 2) ? 2 : 0);
    }
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)(0x600000ULL + start * VM_PAGE_SIZE)) : "memory");
    return (int64_t)(0x600000ULL + start * VM_PAGE_SIZE);
}

#define VM_SHM_VA_BASE 0x08000000ULL
#define VM_SHM_PAGES 128u

static uint64_t shm_frames[VM_SHM_PAGES];
static uint64_t shm_pt_frame;
static int shm_ready;

int knx_vm_shared_init(void) {
    if (shm_ready)
        return 0;
    void *table = knx_alloc_page();
    if (!table)
        return -12;
    uint64_t *pt = (uint64_t *)table;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        pt[i] = 0;
    for (unsigned i = 0; i < VM_SHM_PAGES; ++i) {
        void *frame = knx_alloc_page();
        if (!frame) {
            for (unsigned f = 0; f < i; ++f)
                knx_free_page((void *)(uintptr_t)shm_frames[f]);
            knx_free_page(table);
            return -12;
        }
        uint8_t *bytes = (uint8_t *)frame;
        for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
            bytes[b] = 0;
        shm_frames[i] = (uint64_t)(uintptr_t)frame;
        pt[i] = shm_frames[i] | 7;
    }
    shm_pt_frame = (uint64_t)(uintptr_t)table;
    shm_ready = 1;

    page_directories[0][VM_SHM_VA_BASE >> 21] = shm_pt_frame | 7;
    return 0;
}

static uint64_t *space_page_directory(void *space) {
    if (!space)
        return 0;
    uint64_t *root = (uint64_t *)(uintptr_t)space;
    if (!(root[0] & 1))
        return 0;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(root[0] & ~0xfffULL);
    if (!(l3[0] & 1))
        return 0;
    return (uint64_t *)(uintptr_t)(l3[0] & ~0xfffULL);
}

int64_t knx_vm_map_shared(void *space, uint64_t flags) {
    if (flags & ~1ULL)
        return -22;
    if (!shm_ready)
        return -12;
    uint64_t *l2 = space_page_directory(space);
    if (!l2)
        return -22;
    if ((l2[VM_SHM_VA_BASE >> 21] & ~0xfffULL) != shm_pt_frame)
        return -12;
    return (int64_t)VM_SHM_VA_BASE;
}

#define VM_FB_VA_BASE 0x02000000ULL
#define VM_FB_PAGE_SIZE 0x200000ULL
#define VM_FB_WINDOW_PAGES 32u

static uint64_t fb_window_va;
static uint64_t fb_window_size;

int knx_vm_framebuffer_init(uint64_t physical, uint64_t size) {
    if (!physical || !size || physical + size < physical ||
        size > (uint64_t)VM_FB_WINDOW_PAGES * VM_FB_PAGE_SIZE)
        return -22;
    uint64_t base = physical & ~(VM_FB_PAGE_SIZE - 1);
    uint64_t offset = physical - base;
    if (offset + size > (uint64_t)VM_FB_WINDOW_PAGES * VM_FB_PAGE_SIZE)
        return -22;
    unsigned pages = (unsigned)((offset + size + VM_FB_PAGE_SIZE - 1) / VM_FB_PAGE_SIZE);
    unsigned first = (unsigned)(VM_FB_VA_BASE >> 21);
    for (unsigned i = 0; i < pages; ++i) {
        uint64_t *entry = &page_directories[0][first + i];

        if (!(*entry & 1) || !(*entry & 0x80))
            return -12;

        *entry = (base + (uint64_t)i * VM_FB_PAGE_SIZE) | 0x109fu;
    }
    fb_window_va = VM_FB_VA_BASE + offset;
    fb_window_size = size;
    return 0;
}

int64_t knx_vm_map_framebuffer(void) {
    if (!fb_window_size)
        return -19;
    return (int64_t)fb_window_va;
}

static int vm_walk(uint64_t address, uint64_t *frame, uint64_t *perm, uint64_t **pt_table,
                   unsigned *pte_index) {
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    uint64_t *root = (uint64_t *)(uintptr_t)(cr3 & ~0xfffULL);
    if (!(root[0] & 1))
        return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(root[0] & ~0xfffULL);
    unsigned pd_idx = (unsigned)((address >> 30) & 511);
    uint64_t l2e = pdpt[pd_idx];
    if (!(l2e & 1))
        return 0;
    uint64_t *pd = (uint64_t *)(uintptr_t)(l2e & ~0xfffULL);
    unsigned pt_idx = (unsigned)((address >> 21) & 511);
    if (!(pd[pt_idx] & 1) || (pd[pt_idx] & 0x80))
        return 0;
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pt_idx] & ~0xfffULL);
    unsigned pte_idx = (unsigned)((address >> 12) & 511);
    uint64_t pte = pt[pte_idx];
    *pt_table = pt;
    *pte_index = pte_idx;
    if (!(pte & 1)) {
        *frame = 0;
        *perm = pte;
        return 1;
    }
    *frame = pte & ~0xfffULL;
    *perm = pte;
    return 1;
}

static int vm_install_user_page(uint64_t *pt_table, unsigned pti, uint64_t address,
                                 uint64_t permissions) {
    void *page = knx_alloc_page();
    if (!page)
        return 0;
    uint8_t *bytes = page;
    for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
        bytes[b] = 0;
    uint64_t frame_addr = (uint64_t)(uintptr_t)page;
    pt_table[pti] = frame_addr | permissions;
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
    return 1;
}

int64_t knx_vm_map_user_page(uint64_t address, uint64_t permissions) {
    if (!permissions || (permissions & ~7ULL))
        return -22;
    if (address & (VM_PAGE_SIZE - 1))
        return -22;
    uint64_t dyn_lo = 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_FIRST * VM_PAGE_SIZE;
    uint64_t dyn_hi = 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_LAST * VM_PAGE_SIZE;
    if (address < dyn_lo || address >= dyn_hi)
        return -22;
    uint64_t *pt_table = 0;
    unsigned pte_index = 0;
    uint64_t frame = 0, perm = 0;
    if (!vm_walk(address, &frame, &perm, &pt_table, &pte_index))
        return -22;
    if (pt_table[pte_index] & 1)
        return -22;
    if (!vm_install_user_page(pt_table, pte_index, address, permissions))
        return -12;
    return (int64_t)address;
}

int knx_vm_handle_fault(uint64_t address, uint64_t error_code) {
    uint64_t page_address = address & ~(VM_PAGE_SIZE - 1);
    uint64_t *pt_table = 0;
    unsigned pti = 0;
    uint64_t frame = 0, perm = 0;
    int found = vm_walk(page_address, &frame, &perm, &pt_table, &pti);
    if (found && frame) {
        if ((perm & KNX_PTE_COW) && (error_code & 3) == 3) {

            pt_table[pti] = (perm & ~KNX_PTE_COW) | 2ULL;
            __asm__ volatile("invlpg (%0)" : : "r"(page_address) : "memory");
            return 0;
        }
        return 1;
    }
    if (!found)
        return 1;
    uint64_t dyn_lo = 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_FIRST * VM_PAGE_SIZE;
    uint64_t dyn_hi = 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_LAST * VM_PAGE_SIZE;
    if (page_address < dyn_lo || page_address >= dyn_hi)
        return 1;
    uint64_t permissions = 1  | 4 ;
    if (!(error_code & 1))
        permissions |= 2;
    void *page = knx_alloc_page();
    if (!page)
        return 1;
    uint8_t *bytes = page;
    for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
        bytes[b] = 0;
    pt_table[pti] = (uint64_t)(uintptr_t)page | permissions;
    __asm__ volatile("invlpg (%0)" : : "r"(page_address) : "memory");
    return 0;
}

static uint64_t knx_rd16(const void *p, unsigned off) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)b[off] | ((uint64_t)b[off + 1] << 8);
}
static uint64_t knx_rd32(const void *p, unsigned off) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)b[off] | ((uint64_t)b[off + 1] << 8) | ((uint64_t)b[off + 2] << 16) |
           ((uint64_t)b[off + 3] << 24);
}
static uint64_t knx_rd64(const void *p, unsigned off) {
    return knx_rd32(p, off) | (knx_rd32(p, off + 4) << 32);
}

void *knx_vm_load_elf(const void *image, uint64_t size, uint64_t *end_out, uint64_t *entry_out,
                     uint64_t stack_va, uint64_t stack_frame) {

    if (!image || !size || size > 0x200000)
        return 0;
    const uint8_t *b = (const uint8_t *)image;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        return 0;
    if (b[4] != 2 || b[5] != 1 || b[6] != 1 || knx_rd16(b, 16) != 2 ||
        knx_rd16(b, 18) != 62 || knx_rd32(b, 20) != 1 || knx_rd16(b, 52) < 64)
        return 0;
    uint64_t entry = knx_rd64(b, 24);
    uint64_t phoff = knx_rd64(b, 32);
    uint64_t phentsize = knx_rd16(b, 54);
    uint64_t phnum = knx_rd16(b, 56);
    if (phnum == 0 || phentsize < 56 || phoff > size || phnum > (size - phoff) / phentsize)
        return 0;
    if (entry < 0x400000ULL || entry >= 0x600000ULL)
        return 0;

    uint64_t *root = new_table();
    uint64_t *l3 = new_table();
    uint64_t *l2 = new_table();
    uint64_t *exec_pt = new_table();
    uint64_t *stack_pt = new_table();
    if (!root || !l3 || !l2 || !exec_pt || !stack_pt)
        goto fail;

    uint64_t *kernel_top = (uint64_t *)(uintptr_t)kernel_root;
    uint64_t *kernel_l3 = (uint64_t *)(uintptr_t)(kernel_top[0] & ~0xfffULL);
    uint64_t *kernel_l2 = (uint64_t *)(uintptr_t)(kernel_l3[0] & ~0xfffULL);
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
        root[i] = kernel_top[i];
        l3[i] = kernel_l3[i];
        l2[i] = kernel_l2[i];
    }
    l2[2] = (uint64_t)(uintptr_t)exec_pt | 7;
    l2[3] = (uint64_t)(uintptr_t)stack_pt | 7;
    l3[0] = (uint64_t)(uintptr_t)l2 | 7;
    root[0] = (uint64_t)(uintptr_t)l3 | 7;

    uint64_t end = 0;
    int entry_executable = 0;
    for (uint64_t i = 0; i < phnum; ++i) {
        const uint8_t *p = (const uint8_t *)image + phoff + i * phentsize;
        if (knx_rd32(p, 0) != 1)
            continue;
        uint64_t p_flags = knx_rd32(p, 4);
        uint64_t p_offset = knx_rd64(p, 8);
        uint64_t p_vaddr = knx_rd64(p, 16);
        uint64_t p_filesz = knx_rd64(p, 32);
        uint64_t p_memsz = knx_rd64(p, 40);
        if (p_filesz > size || p_offset > size - p_filesz || p_memsz < p_filesz || !p_memsz)
            goto fail;
        if ((p_vaddr & (VM_PAGE_SIZE - 1)) || p_vaddr < 0x400000ULL ||
            p_vaddr >= 0x600000ULL || p_memsz > 0x600000ULL - p_vaddr)
            goto fail;
        if ((p_flags & ~7ULL) || ((p_flags & 3) == 3))
            goto fail;
        uint64_t first = (p_vaddr - 0x400000ULL) >> 12;
        uint64_t pages = (p_memsz + VM_PAGE_SIZE - 1) / VM_PAGE_SIZE;
        uint64_t perms = 1  | 4 ;
        if (p_flags & 2)
            perms |= 2;
        for (uint64_t page = 0; page < pages; ++page) {
            if (first + page >= PAGE_ENTRIES || (exec_pt[first + page] & 1))
                goto fail;
            void *frame = knx_alloc_page();
            if (!frame)
                goto fail;
            uint8_t *dst = frame;
            for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
                dst[b] = 0;
            exec_pt[first + page] = (uint64_t)(uintptr_t)frame | perms;
        }
        const uint8_t *src = (const uint8_t *)image + p_offset;
        for (uint64_t k = 0; k < p_filesz; ++k) {
            uint64_t page = first + k / VM_PAGE_SIZE;
            uint64_t offset = k % VM_PAGE_SIZE;
            uint8_t *dst = (uint8_t *)(uintptr_t)(exec_pt[page] & 0x000ffffffffff000ULL);
            dst[offset] = src[k];
        }
        uint64_t seg_end = p_vaddr + p_memsz;
        if (seg_end > end)
            end = seg_end;
        if ((p_flags & 1) && entry >= p_vaddr && entry < seg_end)
            entry_executable = 1;
    }

    if (!entry_executable)
        goto fail;

    unsigned stack_index = (unsigned)((stack_va - 0x600000ULL - 1) / VM_PAGE_SIZE);
    if (stack_va <= 0x600000ULL || stack_va > 0x800000ULL || stack_index >= 512 ||
        !stack_frame || (stack_frame & (VM_PAGE_SIZE - 1)))
        goto fail;
    stack_pt[stack_index] = stack_frame | 7;

    if (end_out)
        *end_out = end;
    if (entry_out)
        *entry_out = entry;
    return root;

fail:
    for (unsigned i = 0; i < 512; ++i) {
        if (exec_pt && (exec_pt[i] & 1))
            knx_free_page((void *)(uintptr_t)(exec_pt[i] & ~0xfffULL));
        if (stack_pt && (stack_pt[i] & 1))
            knx_free_page((void *)(uintptr_t)(stack_pt[i] & ~0xfffULL));
    }
    if (exec_pt)
        knx_free_page(exec_pt);
    if (stack_pt)
        knx_free_page(stack_pt);
    if (l2)
        knx_free_page(l2);
    if (l3)
        knx_free_page(l3);
    if (root)
        knx_free_page(root);
    return 0;
}

int knx_vm_unmap(uint64_t base, uint64_t size) {
    if (!size || (base & (VM_PAGE_SIZE - 1)) || base < 0x600000ULL ||
        base >= 0x800000ULL || size > 0x800000ULL - base)
        return -22;
    uint64_t pages = (size + VM_PAGE_SIZE - 1) / VM_PAGE_SIZE;
    unsigned first = (unsigned)((base - 0x600000ULL) / VM_PAGE_SIZE);
    if (first < VM_USER_DYNAMIC_FIRST || first + pages > VM_USER_DYNAMIC_LAST)
        return -22;
    uint64_t *pt = active_user_pt();
    if (!pt)
        return -22;
    for (unsigned n = 0; n < pages; ++n)
        if (!(pt[first + n] & 1))
            return -22;
    for (unsigned n = 0; n < pages; ++n) {
        uint64_t entry = pt[first + n];
        pt[first + n] = 0;
        knx_free_page((void *)(uintptr_t)(entry & ~0xfffULL));
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)(base + n * VM_PAGE_SIZE)) : "memory");
    }
    return 0;
}
