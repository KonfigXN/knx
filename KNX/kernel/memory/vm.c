#include "knx.h"

/*
 * AArch64 (VMSAv8-64) stage-1 translation tables for EL1&0.
 *
 * 4 KiB granule, 48-bit VA, 4 levels:
 *   L0 = VA[47:39], L1 = VA[38:30], L2 = VA[29:21], L3 = VA[20:12].
 *
 * Descriptor encoding used throughout this file:
 *   valid       bit 0
 *   table/page  bit 1   (1 => table descriptor, or page at L3; 0 => block)
 *   AttrIndx    bits[3:2]  (1 = normal WB from MAIR_EL1, 0 = device-nGnRnE)
 *   AP[1]       bit 6      (EL0 access)
 *   AP[2]       bit 7      (read-only)
 *   SH          bits[9:8]  (3 = inner shareable)
 *   AF          bit 10
 *   COW/OWNED   bits 54/55 (software; the architecture ignores them here)
 *
 * Kernels and user spaces share the same table-walk code; only TTBR0_EL1
 * changes on a context switch.  TTBR1_EL1 is disabled (TCR_EL1.EPD1).
 */

#define PAGE_ENTRIES 512u
#define VM_PAGE_SIZE 4096ULL
#define VM_USER_DYNAMIC_FIRST 256u
#define VM_USER_DYNAMIC_LAST  512u

/* Descriptor bits. */
#define PTE_VALID  (1ULL << 0)
#define PTE_PAGE   (1ULL << 1)
#define PTE_NORMAL (1ULL << 2)   /* MAIR_EL1 index 1 */
#define PTE_AP1    (1ULL << 6)   /* user accessible */
#define PTE_AP2    (1ULL << 7)   /* read-only */
#define PTE_SH     (3ULL << 8)   /* inner shareable */
#define PTE_AF     (1ULL << 10)
#define PTE_COW    (1ULL << 54)  /* software */
#define PTE_OWNED  (1ULL << 55)  /* software */
#define PA_MASK    0x0000fffffffff000ULL

#define PTE_TABLE    (PTE_VALID | PTE_PAGE)                       /* 0x003 */
#define BLK_KERN_RW  (PTE_VALID | PTE_NORMAL | PTE_SH | PTE_AF)   /* 0x705 */
#define BLK_USER_RW  (BLK_KERN_RW | PTE_AP1)                      /* 0x745 */
#define BLK_DEV      (PTE_VALID | PTE_SH | PTE_AF)                /* 0x701 */
#define PG_KERN_RW   (BLK_KERN_RW | PTE_PAGE)                     /* 0x707 */
#define PG_USER_RW   (PG_KERN_RW | PTE_AP1)                       /* 0x747 */
/* A user read-only *page* additionally needs AP[2]; 0x745 is the equivalent
 * user block descriptor. */
#define PG_USER_RO   (PG_USER_RW | PTE_AP2)                       /* 0x7c7 */
#define PG_DEV       (BLK_DEV | PTE_PAGE)                         /* 0x703 */

#define VM_SHM_VA_BASE 0x08000000ULL
#define VM_SHM_PAGES   128u
#define VM_FB_VA_BASE  0x02000000ULL
#define VM_FB_PAGE_SIZE 0x200000ULL
#define VM_FB_WINDOW_PAGES 32u

extern char __user_text_start, __user_text_end, __user_stack_start, __user_stack_end;

/* Bootstrap tables.  knx_vm_init() runs before the PMM is up, so the whole
 * identity map must live in static storage. */
static uint64_t l0_table[PAGE_ENTRIES]  __attribute__((aligned(4096)));
static uint64_t l1_low[PAGE_ENTRIES]    __attribute__((aligned(4096)));
static uint64_t l2_low[PAGE_ENTRIES]    __attribute__((aligned(4096)));
static uint64_t l3_low[PAGE_ENTRIES]    __attribute__((aligned(4096)));
static uint64_t l3_stack[PAGE_ENTRIES]  __attribute__((aligned(4096)));
static uint64_t l2_high[PAGE_ENTRIES]   __attribute__((aligned(4096)));
static uint64_t l3_usertext[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_root_phys;

static uint64_t pa_of(uint64_t entry) { return entry & PA_MASK; }
static uint64_t *table_of(uint64_t entry) { return (uint64_t *)(uintptr_t)pa_of(entry); }
static uint64_t phys_of(const void *p) { return (uint64_t)(uintptr_t)p; }
static int is_table(uint64_t entry) { return (entry & 3ULL) == PTE_TABLE; }
static int is_static_table(const uint64_t *t) {
    return t == l0_table || t == l1_low || t == l2_low || t == l3_low ||
           t == l3_stack || t == l2_high || t == l3_usertext;
}

/* dsb sy; tlbi vmalle1; dsb sy; isb -- required after TTBR0 or table edits. */
static void vm_flush(void) {
    __asm__ volatile("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");
}

static uint64_t current_root(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(value));
    return value & PA_MASK;
}

static void install_root(uint64_t phys) {
    __asm__ volatile("msr ttbr0_el1, %0" ::"r"(phys) : "memory");
    vm_flush();
}

/* --- dynamic page allocation ------------------------------------------- */

static uint64_t *new_table(void) {
    uint64_t *table = knx_alloc_page();
    if (!table)
        return 0;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        table[i] = 0;
    return table;
}

#define VM_MMIO_TABLES 16u
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

/* Return the child table at parent[idx], allocating (and, for a pre-existing
 * block, splitting) it when needed.  level 0 entries span 512 GiB, level 1
 * entries span 1 GiB. */
static uint64_t *table_at(uint64_t *parent, unsigned idx, uint64_t base, int level) {
    uint64_t entry = parent[idx];
    if (is_table(entry))
        return table_of(entry);
    uint64_t *child = mmio_table();
    if (!child)
        return 0;
    if (entry & PTE_VALID) {
        uint64_t span = level == 0 ? (512ULL << 30) : (1ULL << 30);
        uint64_t sub = span / PAGE_ENTRIES;
        uint64_t attrs = entry & ~3ULL;
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            child[i] = base + (uint64_t)i * sub + attrs;
    }
    parent[idx] = phys_of(child) | PTE_TABLE;
    return child;
}

/* --- initial identity map / MMU enable --------------------------------- */

void knx_vm_init(void) {
    /* The whole low 4 GiB is identity mapped.  L1[0] and L1[1] are tables;
     * L1[2..] are 1 GiB normal blocks (covers RAM up to 64 GiB). */
    l0_table[0] = phys_of(l1_low) | PTE_TABLE;

    l1_low[0] = phys_of(l2_low) | PTE_TABLE;
    l1_low[1] = phys_of(l2_high) | PTE_TABLE;
    for (unsigned i = 2; i < PAGE_ENTRIES; ++i)
        l1_low[i] = ((uint64_t)i << 30) | BLK_KERN_RW;

    /* L2 for 0..1 GiB: 2 MiB normal blocks with the special windows below. */
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l2_low[i] = ((uint64_t)i << 21) | BLK_KERN_RW;
    l2_low[0] = phys_of(l3_low) | PTE_TABLE;                 /* first 2 MiB */
    l2_low[3] = phys_of(l3_stack) | PTE_TABLE;               /* stack + dynamic */
    for (unsigned i = VM_FB_VA_BASE >> 21;
         i < (VM_FB_VA_BASE >> 21) + VM_FB_WINDOW_PAGES; ++i)
        l2_low[i] = ((uint64_t)i << 21) | BLK_DEV;           /* framebuffer */
    l2_low[0x08000000ULL >> 21] = 0x08000000ULL | BLK_DEV;   /* GIC MMIO */

    /* First 2 MiB: identity 4 KiB pages (legacy low user text area). */
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l3_low[i] = ((uint64_t)i << 12) | PG_KERN_RW;
    for (uint64_t a = phys_of(&__user_text_start); a < phys_of(&__user_text_end);
         a += VM_PAGE_SIZE) {
        unsigned idx = (unsigned)((a >> 12) & (PAGE_ENTRIES - 1));
        if (a < 0x200000ULL)
            l3_low[idx] = a | PG_USER_RO;
    }

    /* User stack at VA 0x600000 (first half), user RW. */
    uint64_t stack_start = phys_of(&__user_stack_start);
    uint64_t stack_end = phys_of(&__user_stack_end);
    for (uint64_t a = stack_start, i = 0; a < stack_end && i < VM_USER_DYNAMIC_FIRST;
         a += VM_PAGE_SIZE, ++i)
        l3_stack[i] = a | PG_USER_RW;

    /* L2 for 1..2 GiB: normal 2 MiB blocks; the block holding .usertext is
     * replaced by a 4 KiB L3 table with user-accessible pages. */
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l2_high[i] = (0x40000000ULL + ((uint64_t)i << 21)) | BLK_KERN_RW;
    l2_high[0] = phys_of(l3_usertext) | PTE_TABLE;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        l3_usertext[i] = (0x40000000ULL + ((uint64_t)i << 12)) | PG_USER_RW;
    for (uint64_t a = phys_of(&__user_text_start); a < phys_of(&__user_text_end);
         a += VM_PAGE_SIZE) {
        unsigned idx = (unsigned)((a - 0x40000000ULL) >> 12);
        if (a >= 0x40000000ULL && a < 0x40200000ULL && idx < PAGE_ENTRIES)
            l3_usertext[idx] = a | PG_USER_RO;   /* user RO + executable */
    }

    kernel_root_phys = phys_of(l0_table);

    /* MAIR_EL1: [0] = device-nGnRnE (0x00), [1] = normal write-back (0xff). */
    __asm__ volatile("msr mair_el1, %0" ::"r"(0xff00ULL) : "memory");

    /* TCR_EL1: T0SZ=16 (48-bit VA), EPD1=1 (TTBR1 off), TG0=0 (4 KiB),
     * SH0=3 (inner shareable), IRGN0/ORGN0=1 (WB, read/write allocate),
     * IPS=5 (48-bit physical addresses). */
    uint64_t tcr = (16ULL << 0) | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |
                   (0ULL << 14) | (1ULL << 23) | (5ULL << 32);
    __asm__ volatile("msr tcr_el1, %0" ::"r"(tcr) : "memory");

    install_root(kernel_root_phys);

    /* Do not trust firmware SCTLR_EL1: explicitly enable M, A, C and I. */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el1, %0" ::"r"(sctlr) : "memory");
    __asm__ volatile("isb" ::: "memory");
    vm_flush();
}

/* --- 1:1 device mappings ----------------------------------------------- */

int knx_vm_map_mmio(uint64_t physical, uint64_t size) {
    if (!size || physical + size < physical)
        return -1;
    uint64_t begin = physical & ~0x1fffffULL;
    uint64_t end = (physical + size + 0x1fffffULL) & ~0x1fffffULL;
    if (end <= begin)
        return -1;
    for (uint64_t a = begin; a < end; a += 0x200000ULL) {
        unsigned i0 = (unsigned)((a >> 39) & (PAGE_ENTRIES - 1));
        uint64_t *l1 = table_at(l0_table, i0, (uint64_t)i0 << 39, 0);
        if (!l1)
            return -1;
        unsigned i1 = (unsigned)((a >> 30) & (PAGE_ENTRIES - 1));
        uint64_t base1 = a & ~((1ULL << 30) - 1);
        uint64_t *l2 = table_at(l1, i1, base1, 1);
        if (!l2)
            return -1;
        unsigned i2 = (unsigned)((a >> 21) & (PAGE_ENTRIES - 1));
        l2[i2] = a | BLK_DEV;
    }
    vm_flush();
    return 0;
}

/* --- address-space cloning / destruction ------------------------------- */

static void copy_page(uint64_t *dst_entry, uint64_t src_entry, uint64_t flags) {
    void *page = knx_alloc_page();
    if (!page)
        return;
    const uint8_t *src = (const uint8_t *)(uintptr_t)pa_of(src_entry);
    uint8_t *dst = page;
    for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
        dst[b] = src[b];
    *dst_entry = phys_of(page) | flags;
}

void *knx_vm_clone_user(void) {
    uint64_t *src_l0 = table_of(current_root());
    uint64_t *src_l1 = is_table(src_l0[0]) ? table_of(src_l0[0]) : 0;
    if (!src_l1)
        return 0;
    uint64_t *src_l2 = is_table(src_l1[0]) ? table_of(src_l1[0]) : 0;
    if (!src_l2)
        return 0;
    uint64_t *src_low = is_table(src_l2[0]) ? table_of(src_l2[0]) : 0;
    uint64_t *src_stack = is_table(src_l2[3]) ? table_of(src_l2[3]) : 0;
    uint64_t *src_exec = is_table(src_l2[2]) ? table_of(src_l2[2]) : 0;

    uint64_t *l0 = new_table();
    uint64_t *l1 = new_table();
    uint64_t *l2 = new_table();
    uint64_t *low = new_table();
    uint64_t *stack = new_table();
    uint64_t *exec = src_exec ? new_table() : 0;
    if (!l0 || !l1 || !l2 || !low || !stack || (src_exec && !exec))
        goto fail;

    for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
        l0[i] = src_l0[i];
        l1[i] = src_l1[i];
        l2[i] = src_l2[i];
    }
    l0[0] = phys_of(l1) | PTE_TABLE;
    l1[0] = phys_of(l2) | PTE_TABLE;
    l2[0] = phys_of(low) | PTE_TABLE;
    l2[3] = phys_of(stack) | PTE_TABLE;
    if (exec)
        l2[2] = phys_of(exec) | PTE_TABLE;

    /* Low L3: copy owned / writable user pages, share the rest. */
    if (src_low) {
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t e = src_low[i];
            int owned = (e & PTE_OWNED) != 0;
            int writable_user = (e & PTE_VALID) && (e & PTE_AP1) && !(e & PTE_AP2);
            if (owned || writable_user) {
                uint64_t flags = (e & ~PTE_COW) | PTE_OWNED;
                flags &= ~PTE_AP2;             /* keep writable */
                copy_page(&low[i], e, flags);
                if (!low[i])
                    goto fail;
            } else {
                low[i] = e;
            }
        }
    }

    /* ELF execution L3: every page is private to the new space. */
    if (src_exec && exec) {
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t e = src_exec[i];
            if (!(e & PTE_VALID))
                continue;
            copy_page(&exec[i], e, (e & ~PTE_COW) | PTE_OWNED);
            if (!exec[i])
                goto fail;
        }
    }

    /* Stack L3: a full private copy of every present page. */
    if (src_stack) {
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t e = src_stack[i];
            if (!(e & PTE_VALID))
                continue;
            copy_page(&stack[i], e, e & ~PTE_COW);
            if (!stack[i])
                goto fail;
        }
    }

    return l0;

fail:
    if (exec)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (exec[i] & PTE_VALID)
                knx_free_page((void *)(uintptr_t)pa_of(exec[i]));
    if (low)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if ((low[i] & PTE_VALID) && (low[i] & PTE_OWNED))
                knx_free_page((void *)(uintptr_t)pa_of(low[i]));
    if (stack)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (stack[i] & PTE_VALID)
                knx_free_page((void *)(uintptr_t)pa_of(stack[i]));
    knx_free_page(exec);
    knx_free_page(stack);
    knx_free_page(low);
    knx_free_page(l2);
    knx_free_page(l1);
    knx_free_page(l0);
    return 0;
}

void knx_vm_destroy_user(void *space) {
    if (!space || phys_of(space) == kernel_root_phys)
        return;
    uint64_t *l0 = space;
    if (!is_table(l0[0]))
        return;
    uint64_t *l1 = table_of(l0[0]);
    if (!is_table(l1[0]))
        return;
    uint64_t *l2 = table_of(l1[0]);
    uint64_t *low = is_table(l2[0]) ? table_of(l2[0]) : 0;
    uint64_t *exec = is_table(l2[2]) ? table_of(l2[2]) : 0;
    uint64_t *stack = is_table(l2[3]) ? table_of(l2[3]) : 0;

    if (exec)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (exec[i] & PTE_VALID)
                knx_free_page((void *)(uintptr_t)pa_of(exec[i]));
    if (low)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if ((low[i] & PTE_VALID) && (low[i] & PTE_OWNED))
                knx_free_page((void *)(uintptr_t)pa_of(low[i]));
    uint64_t stack_lo = phys_of(&__user_stack_start);
    uint64_t stack_hi = phys_of(&__user_stack_end);
    if (stack)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
            if (!(stack[i] & PTE_VALID))
                continue;
            uint64_t frame = pa_of(stack[i]);
            if (frame < stack_lo || frame >= stack_hi)
                knx_free_page((void *)(uintptr_t)frame);
        }

    if (!is_static_table(stack))
        knx_free_page(stack);
    if (!is_static_table(exec))
        knx_free_page(exec);
    if (!is_static_table(low))
        knx_free_page(low);
    if (!is_static_table(l2))
        knx_free_page(l2);
    if (!is_static_table(l1))
        knx_free_page(l1);
    knx_free_page(l0);
}

void knx_vm_switch(void *space) {
    uint64_t root = space ? phys_of(space) : kernel_root_phys;
    install_root(root);
}

uint64_t knx_vm_current_root(void) {
    return current_root();
}

/* --- table walking for the active space -------------------------------- */

static int vm_walk(uint64_t va, uint64_t **pt_out, unsigned *idx_out) {
    uint64_t *l0 = table_of(current_root());
    unsigned i0 = (unsigned)((va >> 39) & (PAGE_ENTRIES - 1));
    if (!is_table(l0[i0]))
        return 0;
    uint64_t *l1 = table_of(l0[i0]);
    unsigned i1 = (unsigned)((va >> 30) & (PAGE_ENTRIES - 1));
    if (!is_table(l1[i1]))
        return 0;
    uint64_t *l2 = table_of(l1[i1]);
    unsigned i2 = (unsigned)((va >> 21) & (PAGE_ENTRIES - 1));
    if (!is_table(l2[i2]))
        return 0;
    *pt_out = table_of(l2[i2]);
    *idx_out = (unsigned)((va >> 12) & (PAGE_ENTRIES - 1));
    return 1;
}

static uint64_t *active_dynamic_pt(void) {
    uint64_t *l0 = table_of(current_root());
    if (!is_table(l0[0]))
        return 0;
    uint64_t *l1 = table_of(l0[0]);
    if (!is_table(l1[0]))
        return 0;
    uint64_t *l2 = table_of(l1[0]);
    return is_table(l2[3]) ? table_of(l2[3]) : 0;
}

static uint64_t dynamic_lo(void) { return 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_FIRST * VM_PAGE_SIZE; }
static uint64_t dynamic_hi(void) { return 0x600000ULL + (uint64_t)VM_USER_DYNAMIC_LAST * VM_PAGE_SIZE; }

static void zero_and_install(uint64_t *entry, uint64_t flags) {
    void *page = knx_alloc_page();
    if (!page)
        return;
    uint8_t *bytes = page;
    for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
        bytes[b] = 0;
    *entry = phys_of(page) | flags;
}

/* --- anonymous memory / user page install ------------------------------ */

int64_t knx_vm_map_anonymous(uint64_t size, uint64_t permissions) {
    if (!size || size > (VM_USER_DYNAMIC_LAST - VM_USER_DYNAMIC_FIRST) * VM_PAGE_SIZE ||
        !(permissions & 1) || (permissions & ~7ULL) ||
        ((permissions & 2) && (permissions & 4)))
        return -22;
    uint64_t pages = (size + VM_PAGE_SIZE - 1) / VM_PAGE_SIZE;
    uint64_t *pt = active_dynamic_pt();
    if (!pt)
        return -12;
    unsigned run = 0, start = VM_USER_DYNAMIC_FIRST;
    for (unsigned i = VM_USER_DYNAMIC_FIRST; i < VM_USER_DYNAMIC_LAST; ++i) {
        if (!(pt[i] & PTE_VALID)) {
            if (!run)
                start = i;
            if (++run == pages)
                goto found;
        } else {
            run = 0;
        }
    }
    return -12;
found:
    ;
    uint64_t flags = (permissions & 2) ? PG_USER_RW : PG_USER_RO;
    for (unsigned n = 0; n < pages; ++n) {
        void *page = knx_alloc_page();
        if (!page) {
            while (n) {
                --n;
                knx_free_page((void *)(uintptr_t)pa_of(pt[start + n]));
                pt[start + n] = 0;
            }
            return -12;
        }
        uint8_t *bytes = page;
        for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
            bytes[b] = 0;
        pt[start + n] = phys_of(page) | flags;
    }
    vm_flush();
    return (int64_t)(0x600000ULL + (uint64_t)start * VM_PAGE_SIZE);
}

int64_t knx_vm_map_user_page(uint64_t address, uint64_t permissions) {
    if (!permissions || (permissions & ~7ULL) || (address & (VM_PAGE_SIZE - 1)))
        return -22;
    if (address < dynamic_lo() || address >= dynamic_hi())
        return -22;
    uint64_t *pt = 0;
    unsigned idx = 0;
    if (!vm_walk(address, &pt, &idx) || (pt[idx] & PTE_VALID))
        return -22;
    uint64_t flags = (permissions & 2) ? PG_USER_RW : PG_USER_RO;
    zero_and_install(&pt[idx], flags);
    if (!(pt[idx] & PTE_VALID))
        return -12;
    vm_flush();
    return (int64_t)address;
}

int knx_vm_unmap(uint64_t base, uint64_t size) {
    if (!size || (base & (VM_PAGE_SIZE - 1)) || base < 0x600000ULL ||
        base >= 0x800000ULL || size > 0x800000ULL - base)
        return -22;
    uint64_t pages = (size + VM_PAGE_SIZE - 1) / VM_PAGE_SIZE;
    unsigned first = (unsigned)((base - 0x600000ULL) / VM_PAGE_SIZE);
    if (first < VM_USER_DYNAMIC_FIRST || first + pages > VM_USER_DYNAMIC_LAST)
        return -22;
    uint64_t *pt = active_dynamic_pt();
    if (!pt)
        return -22;
    for (unsigned n = 0; n < pages; ++n)
        if (!(pt[first + n] & PTE_VALID))
            return -22;
    for (unsigned n = 0; n < pages; ++n) {
        uint64_t entry = pt[first + n];
        pt[first + n] = 0;
        knx_free_page((void *)(uintptr_t)pa_of(entry));
    }
    vm_flush();
    return 0;
}

/* --- faults ------------------------------------------------------------- */

int knx_vm_handle_fault(uint64_t address, uint64_t error_code) {
    uint64_t page_address = address & ~(VM_PAGE_SIZE - 1);
    uint64_t *pt = 0;
    unsigned idx = 0;

    /* ESR_EL1 EC 0x20/0x21: abort from EL0, 0x24/0x25: abort from EL1. */
    unsigned ec = (unsigned)((error_code >> 26) & 0x3fULL);
    if (ec != 0x20 && ec != 0x21 && ec != 0x24 && ec != 0x25)
        return 1;
    int write_fault = (error_code & (1ULL << 6)) != 0;  /* WnR */

    if (!vm_walk(page_address, &pt, &idx))
        return 1;
    uint64_t entry = pt[idx];
    if (entry & PTE_VALID) {
        if ((entry & PTE_COW) && write_fault) {
            uint64_t flags = (entry & ~PTE_COW) | PTE_OWNED;
            flags &= ~PTE_AP2;                       /* writable after copy */
            void *page = knx_alloc_page();
            if (!page)
                return 1;
            const uint8_t *src = (const uint8_t *)(uintptr_t)pa_of(entry);
            uint8_t *dst = page;
            for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
                dst[b] = src[b];
            if (entry & PTE_OWNED)
                knx_free_page((void *)(uintptr_t)pa_of(entry));
            pt[idx] = phys_of(page) | flags;
            vm_flush();
            return 0;
        }
        return 1;
    }

    if (page_address < dynamic_lo() || page_address >= dynamic_hi())
        return 1;
    zero_and_install(&pt[idx], PG_USER_RW);
    if (!(pt[idx] & PTE_VALID))
        return 1;
    vm_flush();
    return 0;
}

/* --- shared memory ------------------------------------------------------ */

static uint64_t shm_frames[VM_SHM_PAGES];
static uint64_t shm_pt_phys;
static int shm_ready;
static uint64_t shm_early_pt[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint8_t shm_early_frames[VM_SHM_PAGES][VM_PAGE_SIZE] __attribute__((aligned(4096)));
static int shm_used_early;

int knx_vm_shared_init(void) {
    if (shm_ready)
        return 0;
    int early = !knx_total_memory();
    if (early && shm_used_early)
        return -12;
    uint64_t *pt = early ? shm_early_pt : new_table();
    if (!pt)
        return -12;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
        pt[i] = 0;

    for (unsigned i = 0; i < VM_SHM_PAGES; ++i) {
        void *frame = early ? shm_early_frames[i] : knx_alloc_page();
        if (!frame) {
            if (!early)
                for (unsigned f = 0; f < i; ++f)
                    knx_free_page((void *)(uintptr_t)shm_frames[f]);
            if (!early)
                knx_free_page(pt);
            return -12;
        }
        uint8_t *bytes = frame;
        for (unsigned b = 0; b < VM_PAGE_SIZE; ++b)
            bytes[b] = 0;
        shm_frames[i] = phys_of(frame);
        pt[i] = shm_frames[i] | PG_USER_RW;
    }
    shm_pt_phys = phys_of(pt);
    shm_used_early = early;
    shm_ready = 1;
    l2_low[VM_SHM_VA_BASE >> 21] = shm_pt_phys | PTE_TABLE;
    vm_flush();
    return 0;
}

int64_t knx_vm_map_shared(void *space, uint64_t flags) {
    if (flags & ~1ULL)
        return -22;
    if (!shm_ready || !space)
        return -12;
    uint64_t *l0 = space;
    if (!is_table(l0[0]))
        return -22;
    uint64_t *l1 = table_of(l0[0]);
    if (!is_table(l1[0]))
        return -22;
    uint64_t *l2 = table_of(l1[0]);
    if (pa_of(l2[VM_SHM_VA_BASE >> 21]) != shm_pt_phys)
        return -12;
    return (int64_t)VM_SHM_VA_BASE;
}

/* --- framebuffer window ------------------------------------------------- */

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
    for (unsigned i = 0; i < pages; ++i)
        l2_low[first + i] = (base + (uint64_t)i * VM_FB_PAGE_SIZE) | BLK_DEV;
    fb_window_va = VM_FB_VA_BASE + offset;
    fb_window_size = size;
    vm_flush();
    return 0;
}

int64_t knx_vm_map_framebuffer(void) {
    if (!fb_window_size)
        return -19;
    return (int64_t)fb_window_va;
}

/* --- user range validation --------------------------------------------- */

int knx_vm_user_readable(uint64_t address, uint64_t length) {
    if (!length || address + length < address || address + length > (1ULL << 48))
        return 0;
    uint64_t *l0 = table_of(current_root());
    for (uint64_t page = address & ~(VM_PAGE_SIZE - 1); page < address + length;
         page += VM_PAGE_SIZE) {
        unsigned i0 = (unsigned)((page >> 39) & (PAGE_ENTRIES - 1));
        unsigned i1 = (unsigned)((page >> 30) & (PAGE_ENTRIES - 1));
        unsigned i2 = (unsigned)((page >> 21) & (PAGE_ENTRIES - 1));
        unsigned i3 = (unsigned)((page >> 12) & (PAGE_ENTRIES - 1));
        if (!is_table(l0[i0]))
            return 0;
        uint64_t *l1 = table_of(l0[i0]);
        if (!is_table(l1[i1]))
            return 0;
        uint64_t *l2 = table_of(l1[i1]);
        if (!is_table(l2[i2]))
            return 0;
        uint64_t *l3 = table_of(l2[i2]);
        uint64_t entry = l3[i3];
        if (!(entry & PTE_VALID) || !(entry & PTE_PAGE) || !(entry & PTE_AP1))
            return 0;
    }
    return 1;
}

/* --- ELF loading -------------------------------------------------------- */

static uint64_t knx_rd16(const void *p, unsigned off) {
    const uint8_t *b = p;
    return (uint64_t)b[off] | ((uint64_t)b[off + 1] << 8);
}
static uint64_t knx_rd32(const void *p, unsigned off) {
    const uint8_t *b = p;
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
    const uint8_t *b = image;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        return 0;
    if (b[4] != 2 || b[5] != 1 || b[6] != 1 || knx_rd16(b, 16) != 2 ||
        knx_rd16(b, 18) != KNX_ELF_MACHINE || knx_rd32(b, 20) != 1 || knx_rd16(b, 52) < 64)
        return 0;
    uint64_t entry = knx_rd64(b, 24);
    uint64_t phoff = knx_rd64(b, 32);
    uint64_t phentsize = knx_rd16(b, 54);
    uint64_t phnum = knx_rd16(b, 56);
    if (phnum == 0 || phentsize < 56 || phoff > size || phnum > (size - phoff) / phentsize)
        return 0;
    if (entry < 0x400000ULL || entry >= 0x600000ULL)
        return 0;

    uint64_t *l0 = new_table();
    uint64_t *l1 = new_table();
    uint64_t *l2 = new_table();
    uint64_t *exec = new_table();
    uint64_t *stack = new_table();
    if (!l0 || !l1 || !l2 || !exec || !stack)
        goto fail;

    uint64_t *k_l0 = table_of(kernel_root_phys);
    uint64_t *k_l1 = is_table(k_l0[0]) ? table_of(k_l0[0]) : 0;
    uint64_t *k_l2 = k_l1 && is_table(k_l1[0]) ? table_of(k_l1[0]) : 0;
    if (!k_l2)
        goto fail;
    for (unsigned i = 0; i < PAGE_ENTRIES; ++i) {
        l0[i] = k_l0[i];
        l1[i] = k_l1[i];
        l2[i] = k_l2[i];
    }
    l0[0] = phys_of(l1) | PTE_TABLE;
    l1[0] = phys_of(l2) | PTE_TABLE;
    l2[2] = phys_of(exec) | PTE_TABLE;
    l2[3] = phys_of(stack) | PTE_TABLE;

    uint64_t end = 0;
    int entry_executable = 0;
    for (uint64_t i = 0; i < phnum; ++i) {
        const uint8_t *p = b + phoff + i * phentsize;
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
        uint64_t perms = (p_flags & 2) ? PG_USER_RW : PG_USER_RO;
        for (uint64_t page = 0; page < pages; ++page) {
            if (first + page >= PAGE_ENTRIES || (exec[first + page] & PTE_VALID))
                goto fail;
            zero_and_install(&exec[first + page], perms);
            if (!(exec[first + page] & PTE_VALID))
                goto fail;
        }
        const uint8_t *src = b + p_offset;
        for (uint64_t k = 0; k < p_filesz; ++k) {
            uint64_t page = first + k / VM_PAGE_SIZE;
            uint64_t offset = k % VM_PAGE_SIZE;
            uint8_t *dst = (uint8_t *)(uintptr_t)pa_of(exec[page]);
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
    if (stack_va <= 0x600000ULL || stack_va > 0x800000ULL || stack_index >= PAGE_ENTRIES ||
        !stack_frame || (stack_frame & (VM_PAGE_SIZE - 1)))
        goto fail;
    stack[stack_index] = stack_frame | PG_USER_RW;

    if (end_out)
        *end_out = end;
    if (entry_out)
        *entry_out = entry;
    return l0;

fail:
    if (exec)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (exec[i] & PTE_VALID)
                knx_free_page((void *)(uintptr_t)pa_of(exec[i]));
    if (stack)
        for (unsigned i = 0; i < PAGE_ENTRIES; ++i)
            if (stack[i] & PTE_VALID)
                knx_free_page((void *)(uintptr_t)pa_of(stack[i]));
    knx_free_page(stack);
    knx_free_page(exec);
    knx_free_page(l2);
    knx_free_page(l1);
    knx_free_page(l0);
    return 0;
}
