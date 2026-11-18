#include "knx.h"
#include "knx_ext.h"

/*
 * AArch64 kernel extension loader.  ELF64/AARCH64 objects are loaded into
 * pages and the following RELA relocations are supported:
 *   R_AARCH64_ABS64, R_AARCH64_RELATIVE, R_AARCH64_CALL26, R_AARCH64_JUMP26,
 *   R_AARCH64_ADR_PREL_PG_HI21 and R_AARCH64_ADD_ABS_LO12_NC.
 * Unknown relocation types are reported and skipped rather than rejecting the
 * module.  Undefined symbols are resolved against the kernel extension API;
 * branch relocations to API symbols go through a small AArch64 PLT stub.
 */

typedef struct { unsigned char ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum,
    shentsize, shnum, shstrndx; } elf64_ehdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size;
    uint32_t link, info; uint64_t align, entsize; } elf64_shdr;
typedef struct { uint32_t name, info; uint64_t value, size; } elf64_sym;
#define SHF_ALLOC 2u
#define SHT_NOBITS 8u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHT_RELA 4u
#define KX_MAX_SECS 32
#define KX_MAX_MODS 16

static uint16_t rd16(const uint8_t *p);
static uint32_t rd32(const uint8_t *p);
static uint64_t rd64(const uint8_t *p);
static void wr32(uint8_t *p, uint32_t v);
static void wr64(uint8_t *p, uint64_t v);

typedef struct knx_module {
    char name[32]; uint32_t version; int in_use;
    uint8_t *sec_mem[KX_MAX_SECS]; uint64_t sec_pages[KX_MAX_SECS];
    uint8_t *stub_mem; uint64_t stub_pages;
    int (*init)(const knx_ext_api_t *); void (*exit)(const knx_ext_api_t *);
} knx_module;
static knx_module kx_mods[KX_MAX_MODS];

static int kx_name_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static void kx_module_free(knx_module *m) {
    for (int i = 0; i < KX_MAX_SECS; i++) if (m->sec_mem[i]) {
        for (uint64_t p = 0; p < m->sec_pages[i]; p++) knx_free_page(m->sec_mem[i] + p * 4096);
        m->sec_mem[i] = 0; m->sec_pages[i] = 0;
    }
    if (m->stub_mem) {
        for (uint64_t p = 0; p < m->stub_pages; p++) knx_free_page(m->stub_mem + p * 4096);
        m->stub_mem = 0; m->stub_pages = 0;
    }
    for (unsigned i = 0; i < sizeof(m->name); i++) m->name[i] = 0;
    m->version = 0; m->init = 0; m->exit = 0; m->in_use = 0;
}

static void kx_print(const char *s) { knx_puts(s); }
static void *kx_alloc(uint64_t size) { return knx_heap_alloc((size_t)size); }

/* The extension API exposes only architecture-neutral services. */
static knx_ext_api_t kx_api = {
    .abi_version = KNX_EXT_ABI_VERSION,
    .struct_size = sizeof(knx_ext_api_t),
    .kprintf = kx_print,
    .kmalloc = kx_alloc,
    .kfree = knx_heap_free,
};

typedef struct { uint64_t offset, info; int64_t addend; } kx_rela;
typedef struct { const uint8_t *img; uint64_t size, shoff; uint16_t shnum, shent;
                 knx_module *mod; uint8_t *stub_next, *stub_end; } kx_reloc_ctx;

static void *knx_ext_api_lookup(const char *n) {
    if (!n) return 0;
#define API_SYM(field) do { const char *a = #field; unsigned i = 0; \
    while (n[i] && a[i] && n[i] == a[i]) i++; \
    if (!n[i] && !a[i]) return (void *)(uintptr_t)kx_api.field; } while (0)
    API_SYM(kprintf); API_SYM(kmalloc); API_SYM(kfree);
#undef API_SYM
    return 0;
}

static uintptr_t kx_module_base(knx_module *m) {
    uintptr_t base = 0;
    for (unsigned i = 0; i < KX_MAX_SECS; ++i)
        if (m->sec_mem[i] && (!base || (uintptr_t)m->sec_mem[i] < base))
            base = (uintptr_t)m->sec_mem[i];
    return base ? base : (uintptr_t)m->stub_mem;
}

/* ldr x16, #8 ; br x16 ; .quad target  (16 bytes, 8-byte aligned). */
static uintptr_t kx_plt_stub(kx_reloc_ctx *c, uintptr_t target) {
    uint8_t *st = c->stub_next;
    uintptr_t misalign = (uintptr_t)st & 7u;
    if (misalign)
        st += 8u - misalign;
    if (st + 16u > c->stub_end)
        return 0;
    static const uint8_t head[8] = {0x50, 0x00, 0x00, 0x58, 0x00, 0x02, 0x1f, 0xd6};
    for (unsigned q = 0; q < 8; ++q)
        st[q] = head[q];
    for (unsigned q = 0; q < 8; ++q)
        st[8 + q] = (uint8_t)(target >> (q * 8));
    c->stub_next = st + 16u;
    knx_dmb();
    knx_isb();
    return (uintptr_t)st;
}

static void kx_skip_reloc(uint32_t type, uint64_t offset) {
    knx_puts("[EXT] skip: unsupported relocation type ");
    knx_print_u64(type);
    knx_puts(" at 0x");
    knx_print_u64(offset);
    knx_puts("\n");
}

static int kx_apply_relocs(kx_reloc_ctx *c) {
    for (uint16_t i = 0; i < c->shnum; i++) {
        const uint8_t *rs = c->img + c->shoff + (uint64_t)i * c->shent;
        if (rd32(rs + 4) != SHT_RELA) continue;
        uint32_t target = rd32(rs + 44);
        if (target >= c->shnum || target >= KX_MAX_SECS || !c->mod->sec_mem[target]) continue;
        uint64_t off = rd64(rs + 24), len = rd64(rs + 32), ents = rd64(rs + 56);
        uint32_t symsec = rd32(rs + 40);
        if (ents != sizeof(kx_rela) || len % ents || off > c->size ||
            len > c->size - off || symsec >= c->shnum) return -1;
        const uint8_t *syh = c->img + c->shoff + (uint64_t)symsec * c->shent;
        uint64_t so = rd64(syh + 24), sl = rd64(syh + 32), se = rd64(syh + 56);
        uint32_t strsec = rd32(syh + 40);
        if (rd32(syh + 4) != SHT_SYMTAB || se < 24 || sl % se ||
            so > c->size || sl > c->size - so || strsec >= c->shnum) return -1;
        const uint8_t *sth = c->img + c->shoff + (uint64_t)strsec * c->shent;
        uint64_t tro = rd64(sth + 24), trl = rd64(sth + 32);
        if (rd32(sth + 4) != SHT_STRTAB || tro > c->size || trl > c->size - tro) return -1;

        const uint8_t *tsh = c->img + c->shoff + (uint64_t)target * c->shent;
        uint64_t targetsize = rd64(tsh + 32);

        for (uint64_t j = 0; j < len / ents; j++) {
            const uint8_t *r = c->img + off + j * ents;
            uint64_t ro = rd64(r), ri = rd64(r + 8);
            int64_t add = (int64_t)rd64(r + 16);
            uint32_t type = (uint32_t)ri, si = (uint32_t)(ri >> 32);

            unsigned width = (type == KNX_R_ABS64 || type == KNX_R_RELATIVE) ? 8u : 4u;
            if (type != KNX_R_ABS64 && type != KNX_R_RELATIVE &&
                type != KNX_R_CALL26 && type != KNX_R_JUMP26 &&
                type != KNX_R_ADRP_HI21 && type != KNX_R_ADD_LO12) {
                kx_skip_reloc(type, ro);
                continue;
            }
            if (ro > targetsize || width > targetsize - ro) return -3;
            uint8_t *P = c->mod->sec_mem[target] + ro;

            uintptr_t S = 0;
            int undef = 0;
            if (type != KNX_R_RELATIVE) {
                if (si >= sl / se) return -4;
                const uint8_t *sym = c->img + so + (uint64_t)si * se;
                uint32_t sn = rd32(sym);
                uint16_t ndx = rd16(sym + 6);
                uint64_t value = rd64(sym + 8);
                if (sn >= trl) return -5;
                uint64_t z = sn;
                while (z < trl && c->img[tro + z]) z++;
                if (z == trl) return -5;
                const char *name = (const char *)c->img + tro + sn;
                if (ndx == 0) {
                    S = (uintptr_t)knx_ext_api_lookup(name);
                    undef = 1;
                    if (!S) {
                        knx_puts("[EXT] skip: unresolved symbol ");
                        knx_puts(name);
                        knx_puts("\n");
                        continue;
                    }
                } else if (ndx == 0xfff1) {
                    S = (uintptr_t)value;
                } else {
                    if (ndx >= c->shnum || ndx >= KX_MAX_SECS ||
                        !c->mod->sec_mem[ndx] ||
                        value > rd64(c->img + c->shoff + (uint64_t)ndx * c->shent + 32))
                        return -7;
                    S = (uintptr_t)c->mod->sec_mem[ndx] + value;
                }
            }

            switch (type) {
            case KNX_R_ABS64:
                wr64(P, (uint64_t)S + (uint64_t)add);
                break;
            case KNX_R_RELATIVE:
                wr64(P, (uint64_t)kx_module_base(c->mod) + (uint64_t)add);
                break;
            case KNX_R_CALL26:
            case KNX_R_JUMP26: {
                uintptr_t dest = S;
                if (undef) {
                    dest = kx_plt_stub(c, S);
                    if (!dest) return -8;
                }
                int64_t v = (int64_t)dest + add - (int64_t)(uintptr_t)P;
                if (v < -(1LL << 27) || v >= (1LL << 27)) {
                    kx_skip_reloc(type, ro);
                    continue;
                }
                uint32_t insn = rd32(P) & 0xfc000000u;
                insn |= (uint32_t)((v >> 2) & 0x03ffffffu);
                wr32(P, insn);
                knx_dmb();
                knx_isb();
                break;
            }
            case KNX_R_ADRP_HI21: {
                if (undef) { kx_skip_reloc(type, ro); continue; }
                int64_t target_addr = (int64_t)S + add;
                int64_t page = target_addr & ~0xfffLL;
                int64_t here = (int64_t)(uintptr_t)P & ~0xfffLL;
                int64_t delta = page - here;
                if (delta < -(1LL << 32) || delta >= (1LL << 32)) {
                    kx_skip_reloc(type, ro);
                    continue;
                }
                uint64_t imm = (uint64_t)(delta >> 12);
                uint32_t immlo = (uint32_t)(imm & 3u);
                uint32_t immhi = (uint32_t)((imm >> 2) & 0x7ffffu);
                uint32_t insn = rd32(P) & 0x9f00001fu;
                insn |= (immhi << 5) | (immlo << 29);
                wr32(P, insn);
                knx_dmb();
                knx_isb();
                break;
            }
            case KNX_R_ADD_LO12: {
                if (undef) { kx_skip_reloc(type, ro); continue; }
                uint32_t imm12 = (uint32_t)(((uint64_t)S + (uint64_t)add) & 0xfffu);
                uint32_t insn = rd32(P) & 0xffc003ffu;
                insn |= imm12 << 10;
                wr32(P, insn);
                knx_dmb();
                break;
            }
            default:
                break;
            }
        }
    }
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p+4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static int bounded_string(const uint8_t *table, uint64_t length, uint32_t offset, const char *wanted) {
    if (offset >= length) return 0;
    uint64_t i = offset; unsigned j = 0;
    while (i < length && table[i] && wanted[j] && table[i] == (uint8_t)wanted[j]) { ++i; ++j; }
    return i < length && table[i] == 0 && wanted[j] == 0;
}
static int has_terminator(const char *s, unsigned size) {
    for (unsigned i = 0; i < size; i++) if (!s[i]) return 1;
    return 0;
}

static int validate(const uint8_t *b, uint64_t size, const char **name, uint32_t *version) {
    if (!b || size < sizeof(elf64_ehdr) || b[0]!=0x7f || b[1]!='E' || b[2]!='L' || b[3]!='F' ||
        b[4]!=2 || b[5]!=1 || b[6]!=1 || rd16(b+16)!=1 || rd16(b+18)!=KNX_ELF_MACHINE ||
        rd32(b+20)!=1 || rd16(b+52)<64)
        return -1;
    uint64_t shoff=rd64(b+40); uint16_t shentsize=rd16(b+58), shnum=rd16(b+60), names=rd16(b+62);
    if (shentsize < sizeof(elf64_shdr) || !shnum || names >= shnum || shoff > size ||
        shnum > (size-shoff)/shentsize) {
        knx_puts("[EXT] reject: section table bounds (shnum="); knx_print_u64(shnum);
        knx_puts(" idx="); knx_print_u64(names); knx_puts(" entsize="); knx_print_u64(shentsize);
        knx_puts(" shoff="); knx_print_u64(shoff); knx_puts(" size="); knx_print_u64(size); knx_puts(")\n");
        return -2;
    }
    const uint8_t *strh=b+shoff+(uint64_t)names*shentsize;
    uint64_t stroff=rd64(strh+24), strsize=rd64(strh+32);
    if (stroff>size || strsize>size-stroff) return -3;
    const uint8_t *str=b+stroff; const uint8_t *meta=0; uint64_t metasize=0;
    for (uint16_t i=0;i<shnum;i++) {
        const uint8_t *s=b+shoff+(uint64_t)i*shentsize;
        uint32_t no=rd32(s); uint64_t off=rd64(s+24), n=rd64(s+32);
        if (rd32(s+4)!=SHT_NOBITS && (off>size || n>size-off)) return -4;
        if (no>=strsize) continue;
        uint64_t j=no; while(j<strsize && str[j]) ++j;
        if(j==strsize) return -5;
        static const char wanted[]=".knx_ext"; unsigned k=0;
        while(k<sizeof(wanted)-1 && no+k<j && str[no+k]==wanted[k]) ++k;
        if(k==sizeof(wanted)-1 && no+k==j){ meta=b+off; metasize=n; }
    }
    if(!meta || metasize<sizeof(knx_ext_header_t)) return -6;
    const knx_ext_header_t *h=(const knx_ext_header_t *)meta;
    if(h->magic!=KNX_EXT_MAGIC || h->header_size!=sizeof(*h) || h->abi_version!=KNX_EXT_ABI_VERSION) return -7;
    if(!h->name[0] || !h->init_symbol[0] || !h->exit_symbol[0] ||
       !has_terminator(h->name,sizeof(h->name)) ||
       !has_terminator(h->init_symbol,sizeof(h->init_symbol)) ||
       !has_terminator(h->exit_symbol,sizeof(h->exit_symbol))) return -8;
    *name=h->name; *version=h->version; return 0;
}

static int load_sections(const uint8_t *b, uint64_t size, knx_module *m) {
    const char *nm=0; uint32_t ver=0; int rc=validate(b,size,&nm,&ver); if(rc)return rc;
    uint64_t shoff=rd64(b+40); uint16_t es=rd16(b+58), count=rd16(b+60), stridx=rd16(b+62);
    if(count>KX_MAX_SECS)return -20;
    const uint8_t *shstr=b+shoff+(uint64_t)stridx*es; uint64_t stroff=rd64(shstr+24), strsize=rd64(shstr+32);
    const uint8_t *meta=0;
    for(uint16_t i=0;i<count;i++){
        const uint8_t *s=b+shoff+(uint64_t)i*es; uint32_t type=rd32(s+4);
        uint64_t flags=rd64(s+8), len=rd64(s+32), align=rd64(s+48), off=rd64(s+24);
        if (bounded_string(b+stroff,strsize,rd32(s),".knx_ext")) meta=b+off;
        if(!(flags&SHF_ALLOC)||!len)continue;
        if (align>4096||(align&&(align&(align-1))))return -21;
        uint64_t pages=(len+4095)/4096; if(pages>32)return -22;
        uint8_t *mem=knx_alloc_page(); if(!mem)return -23;
        m->sec_mem[i]=mem; m->sec_pages[i]=1;
        for(uint64_t p=1;p<pages;p++){uint8_t *q=knx_alloc_page();if(!q)return -24;if(q!=mem+p*4096){knx_free_page(q);return -25;}m->sec_pages[i]++;}
        for(uint64_t j=0;j<pages*4096;j++)mem[j]=0;
        if(type!=SHT_NOBITS)for(uint64_t j=0;j<len;j++)mem[j]=b[off+j];
    }
    if(!meta)return -26;
    const knx_ext_header_t *h=(const knx_ext_header_t *)meta;
    for(unsigned q=0;q<31&&h->name[q];q++)m->name[q]=h->name[q];
    m->version=h->version;

    const uint8_t *symsec=0,*strsec=0; uint64_t symlen=0,symoff=0,symesz=0,strsz=0,stro=0;
    for(uint16_t i=0;i<count;i++){const uint8_t *s=b+shoff+(uint64_t)i*es;uint32_t t=rd32(s+4);
        if(t==SHT_SYMTAB){uint32_t link=rd32(s+40);if(link>=count)return -27;const uint8_t *st=b+shoff+(uint64_t)link*es;
            symsec=s;symoff=rd64(s+24);symlen=rd64(s+32);symesz=rd64(s+56);strsec=st;stro=rd64(st+24);strsz=rd64(st+32);break;}}
    if(!symsec||!strsec||symesz<24||symlen%symesz||symoff>size||symlen>size-symoff||
       rd32(strsec+4)!=SHT_STRTAB||stro>size||strsz>size-stro)return -28;
    uint64_t init=0,exit=0;
    for(uint64_t j=0;j<symlen/symesz;j++){const uint8_t *s=b+symoff+j*symesz;uint32_t no=rd32(s);uint16_t ndx=rd16(s+6);uint64_t val=rd64(s+8);
        if(no>=strsz)return -29;
        uint64_t z=no;while(z<strsz&&b[stro+z])z++;if(z==strsz)return -30;
        if(ndx==0||ndx==0xfff1)continue;
        if(ndx>=count)return -31;
        if(ndx>=KX_MAX_SECS||!m->sec_mem[ndx]||val>=rd64(b+shoff+(uint64_t)ndx*es+32))return -32;
        if(bounded_string(b+stro,strsz,no,h->init_symbol))init=(uintptr_t)m->sec_mem[ndx]+val;
        if(bounded_string(b+stro,strsz,no,h->exit_symbol))exit=(uintptr_t)m->sec_mem[ndx]+val;}
    if(!init||!exit)return -33;
    m->init=(int (*)(const knx_ext_api_t *))(uintptr_t)init;
    m->exit=(void (*)(const knx_ext_api_t *))(uintptr_t)exit;

    m->stub_mem=knx_alloc_page(); if(!m->stub_mem)return -34; m->stub_pages=1;
    kx_reloc_ctx ctx={.img=b,.size=size,.shoff=shoff,.shnum=count,.shent=es,.mod=m,
        .stub_next=m->stub_mem,.stub_end=m->stub_mem+4096};
    rc=kx_apply_relocs(&ctx); if(rc)return rc;
    return 0;
}

int knx_kext_load_file(const char *path) {
    struct knx_vnode *node=0; struct knx_vfs_stat st={0};
    if(knx_vfs_resolve(path,&node) || !node || knx_vfs_stat(node,&st) || !st.size || st.size>4096) return -1;
    uint8_t *buf=knx_alloc_page();
    if(!buf) return -2;
    uint64_t got=0;
    while(got<st.size){ uint64_t take=st.size-got;
        int64_t n=knx_vfs_read(node,got,buf+got,take); if(n!=(int64_t)take){knx_free_page(buf);return -3;} got+=take; }
    knx_module *m=0; for(unsigned i=0;i<KX_MAX_MODS;i++)if(!kx_mods[i].in_use){m=&kx_mods[i];break;}
    if(!m){knx_free_page(buf);return -4;} m->in_use=1;
    int rc=load_sections(buf,st.size,m);
    if (rc) { knx_puts("[EXT] section load rejected: "); knx_print_i64(rc); knx_puts("\n"); }
    knx_free_page(buf);
    if(rc){kx_module_free(m);return rc;}
    if(m->init(&kx_api)){kx_module_free(m);return -5;}
    knx_puts("[EXT] loaded ");knx_puts(m->name);knx_puts(" v");knx_print_u64(m->version);knx_puts("\n");return 0;
}

int knx_kext_unload(const char *name) {
    for (unsigned i = 0; i < KX_MAX_MODS; ++i) {
        knx_module *m = &kx_mods[i];
        if (!m->in_use || !kx_name_equal(m->name, name))
            continue;
        if (m->exit)
            m->exit(&kx_api);
        knx_puts("[EXT] unloaded ");
        knx_puts(m->name);
        knx_puts(" v");
        knx_print_u64(m->version);
        knx_puts("\n");
        kx_module_free(m);
        return 0;
    }
    return -1;
}

int knx_kext_selftest(void) {
    struct knx_vnode *node=0; struct knx_vfs_stat st={0};
    if(knx_vfs_resolve("/sys/ext/hello.kext",&node) || knx_vfs_stat(node,&st) || st.size>32768) return 0;
    if(st.size>4096) return 0;
    uint8_t *b=knx_alloc_page(); if(!b) return 0;
    if(knx_vfs_read(node,0,b,st.size)!=(int64_t)st.size){knx_free_page(b);return 0;}
    const char *n=0; uint32_t v=0;
    uint64_t baseline=knx_free_memory();
    int ok=knx_kext_load_file("/sys/ext/hello.kext")==0;
    uint64_t free_before=knx_free_memory();
    uint8_t saved=b[0]; b[0]=0; ok &= validate(b,st.size,&n,&v)!=0; b[0]=saved;
    ok &= validate(b,st.size-1,&n,&v)!=0;
    if(st.size>=64){uint8_t t=b[40];b[40]=0xff;ok &= validate(b,st.size,&n,&v)!=0;b[40]=t;}
    uint64_t shoff=rd64(b+40); uint16_t es=rd16(b+58), count=rd16(b+60);
    for(uint16_t i=0;i<count;i++){
        uint8_t *s=b+shoff+(uint64_t)i*es;
        if(rd32(s+4)==SHT_SYMTAB){uint32_t old=rd32(s+40);uint64_t before=knx_free_memory();
            s[40]=0xff;s[41]=0xff;s[42]=0;s[43]=0;
            knx_module test;uint8_t *tb=(uint8_t *)&test;for(unsigned q=0;q<sizeof(test);q++)tb[q]=0;test.in_use=1;
            ok &= load_sections(b,st.size,&test)!=0;
            kx_module_free(&test);
            ok &= knx_free_memory()==before;
            s[40]=(uint8_t)old;s[41]=(uint8_t)(old>>8);s[42]=(uint8_t)(old>>16);s[43]=(uint8_t)(old>>24);
            uint64_t oldoff=rd64(s+24);before=knx_free_memory();
            for(unsigned q=0;q<8;q++)s[24+q]=0xff;
            for(unsigned q=0;q<sizeof(test);q++)tb[q]=0;
            test.in_use=1;
            ok &= load_sections(b,st.size,&test)!=0;
            kx_module_free(&test);
            ok &= knx_free_memory()==before;
            for(unsigned q=0;q<8;q++)s[24+q]=(uint8_t)(oldoff>>(q*8));
            break;}
    }
    ok &= knx_free_memory()==free_before;
    ok &= knx_kext_unload("hello")==0;
    ok &= knx_free_memory()==baseline;
    knx_free_page(b); return ok;
}
