#include "knx.h"

struct mount_entry { char path[KNX_VFS_PATH_MAX]; char type[16]; const struct knx_vfs_ops *ops; void *data; struct knx_vnode *root; int used; };
static struct mount_entry mounts[KNX_VFS_MOUNTS_MAX];
static unsigned mount_count;
static int eq(const char *a,const char *b){unsigned i=0;while(a[i]&&a[i]==b[i])++i;return a[i]==b[i];}
static int copy(char*d,unsigned z,const char*s){unsigned i=0;if(!s)return 0;while(s[i]&&i+1<z){d[i]=s[i];++i;}if(s[i])return 0;d[i]=0;return 1;}
static int boundary_prefix(const char*p,const char*b){unsigned i=0;while(b[i]&&p[i]==b[i])++i;return !b[i]&&(i==1||p[i]=='/'||p[i]==0);}
static struct mount_entry *find_mount(const char*p){struct mount_entry*r=0;unsigned best=0;for(unsigned i=0;i<KNX_VFS_MOUNTS_MAX;i++)if(mounts[i].used&&boundary_prefix(p,mounts[i].path)){unsigned n=0;while(mounts[i].path[n])++n;if(n>best){r=&mounts[i];best=n;}}return r;}
void knx_vfs_init(void){for(unsigned i=0;i<KNX_VFS_MOUNTS_MAX;i++)mounts[i].used=0;mount_count=0;}
int knx_vfs_mount(const char*path,const char*type,const struct knx_vfs_ops*ops,void*dev){if(!path||path[0]!='/'||!type||!ops||!ops->mount||!ops->lookup||mount_count>=KNX_VFS_MOUNTS_MAX)return -1;unsigned n=0;while(path[n]&&n<KNX_VFS_PATH_MAX)++n;if(path[n] || (n > 1 && path[n-1] == '/'))return -1;for(unsigned i=0;i<KNX_VFS_MOUNTS_MAX;i++)if(mounts[i].used&&eq(path,mounts[i].path))return -1;unsigned slot=0;while(slot<KNX_VFS_MOUNTS_MAX&&mounts[slot].used)++slot;struct mount_entry*m=&mounts[slot];if(!copy(m->path,sizeof(m->path),path)||!copy(m->type,sizeof(m->type),type))return -1;m->ops=ops;m->root=0;m->data=0;if(ops->mount(dev,&m->data,&m->root)||!m->root)return -1;m->used=1;++mount_count;return 0;}
int knx_vfs_unmount(const char*path){if(!path)return -1;for(unsigned i=0;i<KNX_VFS_MOUNTS_MAX;i++)if(mounts[i].used&&eq(path,mounts[i].path)){if(!mounts[i].ops->unmount||mounts[i].ops->unmount(mounts[i].data))return -1;mounts[i].used=0;--mount_count;return 0;}return -1;}
int knx_vfs_mount_count(void){return (int)mount_count;}
int knx_vfs_mount_info(unsigned i,char*path,unsigned psz,char*type,unsigned tsz){if(i>=mount_count)return -1;for(unsigned k=0;k<KNX_VFS_MOUNTS_MAX;k++)if(mounts[k].used){if(i--==0)return copy(path,psz,mounts[k].path)&&copy(type,tsz,mounts[k].type)?0:-1;}return -1;}
int knx_vfs_normalize(const char *path, char *canonical, unsigned outsz) {
    if (!path || !outsz || !canonical || path[0] != '/')
        return KNX_VFS_EINVAL;
    unsigned length = 0;
    while (path[length]) {
        if (length + 1 >= KNX_VFS_PATH_MAX)
            return KNX_VFS_ENAMETOOLONG;
        ++length;
    }
    unsigned marks[KNX_VFS_PATH_MAX / 2];
    unsigned clen = 1, depth = 0;
    canonical[0] = '/';
    if (outsz < 2) return KNX_VFS_ENAMETOOLONG;
    canonical[1] = 0;
    for (unsigned i = 1; i < length;) {
        while (path[i] == '/') ++i;
        if (!path[i]) break;
        unsigned start = i;
        while (path[i] && path[i] != '/') ++i;
        unsigned n = i - start;
        if (n >= KNX_VFS_NAME_MAX) return KNX_VFS_ENAMETOOLONG;
        if (n == 1 && path[start] == '.') continue;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (depth) { clen = marks[--depth]; canonical[clen] = 0; }
            continue;
        }
        if (depth >= KNX_VFS_PATH_MAX / 2 || clen + n + (clen > 1) >= outsz)
            return KNX_VFS_ENAMETOOLONG;
        marks[depth++] = clen;
        if (clen > 1) canonical[clen++] = '/';
        for (unsigned j = 0; j < n; ++j) canonical[clen++] = path[start + j];
        canonical[clen] = 0;
    }
    return 0;
}

int knx_vfs_resolve(const char *path, struct knx_vnode **out) {
    char canonical[KNX_VFS_PATH_MAX];
    if (!out || knx_vfs_normalize(path, canonical, sizeof(canonical))) return -1;

    struct mount_entry *mount = find_mount(canonical);
    if (!mount)
        return -1;
    struct knx_vnode *node = mount->root;
    unsigned mount_length = 0;
    while (mount->path[mount_length])
        ++mount_length;
    unsigned i = mount_length;
    if (canonical[i] == '/')
        ++i;
    while (canonical[i]) {
        char component[KNX_VFS_NAME_MAX];
        unsigned component_length = 0;
        while (canonical[i] && canonical[i] != '/')
            component[component_length++] = canonical[i++];
        component[component_length] = 0;
        if (canonical[i] == '/')
            ++i;
        if (!node->ops || !node->ops->lookup)
            return -1;
        struct knx_vnode *child = 0;
        if (node->ops->lookup(node, component, &child) || !child)
            return -1;
        node = child;
    }
    *out = node;
    return 0;
}

void knx_vfs_selftest(void) {
    static const struct { const char *name, *path, *expected; int code; } tests[] = {
        {"root", "/", "/", 0}, {"dot", "/./", "/", 0},
        {"dotdot", "/a/../", "/", 0}, {"slashes", "//a///b", "/a/b", 0},
        {"trailing", "/a/", "/a", 0}, {"mixed", "/a/./b/../c", "/a/c", 0},
        {"root_clamp", "/../../", "/", 0}, {"root_clamp_nested", "/a/b/../../..", "/", 0},
        {"empty", "", "", KNX_VFS_EINVAL}, {"relative", "a", "", KNX_VFS_EINVAL},
        {"null", 0, "", KNX_VFS_EINVAL},
    };
    unsigned passed = 0;
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        char output[KNX_VFS_PATH_MAX];
        output[0] = 0;
        int got = knx_vfs_normalize(tests[i].path, output, sizeof(output));
        int ok = got == tests[i].code && eq(output, tests[i].expected);
        knx_puts(ok ? "PASS " : "FAIL ");
        knx_puts(tests[i].name);
        if (!ok) { knx_puts(" (got "); knx_print_i64(got); knx_putc(' '); knx_puts(output);
            knx_puts(", expected "); knx_print_i64(tests[i].code); knx_putc(' '); knx_puts(tests[i].expected); knx_putc(')'); }
        else ++passed;
        knx_putc('\n');
    }
    char long_component[301];
    long_component[0] = '/';
    for (unsigned i = 1; i < 300; ++i)
        long_component[i] = 'x';
    long_component[300] = 0;
    char output[KNX_VFS_PATH_MAX];
    output[0]=0;
    int got=knx_vfs_normalize(long_component,output,sizeof(output));
    int ok=got==KNX_VFS_ENAMETOOLONG; knx_puts(ok?"PASS long_component\n":"FAIL long_component\n"); passed+=ok;
    char long_path[KNX_VFS_PATH_MAX + 1];
    for (unsigned i = 0; i < KNX_VFS_PATH_MAX; ++i)
        long_path[i] = '/';
    long_path[KNX_VFS_PATH_MAX] = 0;
    got = knx_vfs_normalize(long_path, output, sizeof(output));
    ok = got == KNX_VFS_ENAMETOOLONG;
    knx_puts(ok ? "PASS long_path\n" : "FAIL long_path\n");
    passed += ok;
    char boundary[KNX_VFS_PATH_MAX];
    for (unsigned i = 0; i < KNX_VFS_PATH_MAX - 1; ++i)
        boundary[i] = (i % 2) ? 'a' : '/';
    boundary[KNX_VFS_PATH_MAX - 1] = 0;
    got = knx_vfs_normalize(boundary, output, sizeof(output));
    ok = got == 0;
    knx_puts(ok ? "PASS path_boundary\n" : "FAIL path_boundary\n");
    passed += ok;
    knx_puts("vfstest: "); knx_print_u64(passed); knx_puts("/");
    knx_print_u64(sizeof(tests) / sizeof(tests[0]) + 3); knx_puts(" passed\n");
}
int64_t knx_vfs_read(struct knx_vnode*n,uint64_t o,void*b,uint64_t c){return n&&n->ops&&n->ops->read?n->ops->read(n,o,b,c):-1;}
int knx_vfs_readdir(struct knx_vnode*n,uint64_t i,struct knx_vfs_dirent*e){return n&&n->ops&&n->ops->readdir?n->ops->readdir(n,i,e):-1;}
int knx_vfs_stat(struct knx_vnode*n,struct knx_vfs_stat*s){return n&&n->ops&&n->ops->stat?n->ops->stat(n,s):-1;}
