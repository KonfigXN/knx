#include "knx.h"
#include "knx_syscall.h"

#define MAX_TASKS 16
#define MAX_CPUS 16
#define USER_STACK_VIRTUAL_BASE 0x600000ULL
#define USER_STACK_SIZE 4096ULL
#define TASK_UNUSED 0
#define TASK_RUNNABLE 1
#define TASK_EXITED 2
#define TASK_RUNNING 3
#define TASK_BLOCKED 4
#define TASK_ZOMBIE 5
#define TASK_EXEC_PENDING 6

#define TASK_SLEEPING 7

#define PROCESS_MAX_SYSCALLS 1000000ULL
#define PROCESS_MAX_OUTPUT 65536ULL
#define PROCESS_MAX_MAPPED  (1024ULL * 1024ULL)
#define EXEC_MAX_VECTOR 16
#define EXEC_MAX_STRINGS 1024

#define CONSOLE_LINE_MAX 128

static void user_exec_test_build_and_run(void);

__attribute__((section(".usertext"), noinline)) static void user_worker(void) {
    for (;;) {
        (void)knx_sys_ticks();
        for (volatile unsigned i = 0; i < 4000000; ++i)
            knx_pause();
    }
}

__attribute__((section(".usertext"), noinline)) static void user_worker_a(void) {
    char marker;
    int32_t status = -1;
    int64_t child = knx_sys_fork();
    if (child == 0)
        knx_sys_exit(42);
    marker = child > 0 ? '1' : '0';
    if (child > 0) {
        int64_t got = knx_sys_waitpid(child, &status, 0);
        marker = got == child ? (status == 42 ? '2' : '3') : '4';
        if (got == child && status == 42 && knx_sys_waitpid(child, 0, 1) == -10 &&
            knx_sys_waitpid(0x7fffffff, 0, 1) == -10)
            marker = 'P';
        else
            marker = 'F';
    }
    (void)knx_sys_write(&marker, 1);
    user_worker();
}
__attribute__((section(".usertext"), noinline)) static void user_worker_b(void) {
    user_exec_test_build_and_run();
    user_worker();
}

static const char init_online[] __attribute__((section(".usertext.data"))) =
    "KNX init: PID 1 online; kernel console and logging are available.\n";
static const char init_path[] __attribute__((section(".usertext.data"))) = "/bin/init";
static const char init_exec_calling[] __attribute__((section(".usertext.data"))) =
    "KNX init: calling execve /bin/init\n";

__attribute__((section(".usertext"), noinline)) static void user_init(void) {
    (void)knx_sys_write(init_online, sizeof(init_online) - 1);
    char marker = knx_sys_getpid() == 1 ? 'I' : 'i';
    (void)knx_sys_write(&marker, 1);

    const char *argv[2];
    argv[0] = init_path;
    argv[1] = 0;
    const char *envp[1];
    envp[0] = 0;

    (void)knx_sys_write(init_exec_calling, sizeof(init_exec_calling) - 1);
    if (knx_sys_execve(init_path, argv, envp) < 0) {
        static const char failed[] __attribute__((section(".usertext.data"))) =
            "KNX init: unable to execute /bin/init; bootstrap remains active.\n";
        (void)knx_sys_write(failed, sizeof(failed) - 1);
        user_worker_a();
    }
    for (;;)
        (void)knx_sys_yield();
}

static uint8_t kernel_stacks[MAX_TASKS][4096] __attribute__((aligned(4096)));
static uint8_t user_stacks[MAX_TASKS][4096]
    __attribute__((used, aligned(4096), section(".userstack")));

#define KNX_STACK_CANARY 0xC7C7C7C7C7C7C7C7ULL
#define KNX_STACK_CANARY_WORDS 8
static uint8_t stack_armed[MAX_TASKS];

static void stack_arm(unsigned slot) {
    if (slot >= MAX_TASKS)
        return;
    uint64_t *base = (uint64_t *)(uintptr_t)&kernel_stacks[slot][0];
    for (unsigned i = 0; i < KNX_STACK_CANARY_WORDS; ++i)
        base[i] = KNX_STACK_CANARY;
    stack_armed[slot] = 1;
}

static int stack_intact(unsigned slot) {
    if (slot >= MAX_TASKS || !stack_armed[slot])
        return 1;
    uint64_t *base = (uint64_t *)(uintptr_t)&kernel_stacks[slot][0];
    for (unsigned i = 0; i < KNX_STACK_CANARY_WORDS; ++i)
        if (base[i] != KNX_STACK_CANARY)
            return 0;
    return 1;
}

static void dbg_report_stack_smash(unsigned slot) {
    uint64_t *base = (uint64_t *)(uintptr_t)&kernel_stacks[slot][0];
    uint64_t rsp = knx_read_sp();
    knx_puts("DBG kernel stack smashed slot=");
    knx_print_u64(slot);
    knx_puts(" rsp=");
    knx_print_u64(rsp);
    knx_puts(" ktop=");
    knx_print_u64(knx_kernel_stack_top());
    knx_puts(" kbase=");
    knx_print_u64((uint64_t)(uintptr_t)&kernel_stacks[0][0]);
    knx_puts(" base=");
    knx_print_u64((uint64_t)(uintptr_t)base);
    knx_puts(" top=");
    knx_print_u64((uint64_t)(uintptr_t)(base + 512));
    knx_puts("\n  canary:");
    for (unsigned i = 0; i < KNX_STACK_CANARY_WORDS; ++i) {
        knx_puts(" ");
        knx_print_u64(base[i]);
    }
    knx_puts("\n");
}

#define KNX_FD_MAX 16
struct knx_fd {
    struct knx_vnode *node;
    uint64_t offset;
    uint64_t size;
    uint8_t open;
};

struct task {
    uint64_t *frame;
    void *address_space;
    void *retired_space;
    uint64_t pid;
    uint64_t tid;
    uint64_t parent_pid;
    uint64_t syscalls;
    uint64_t cpu_ticks;
    uint64_t output_bytes;
    uint64_t mapped_bytes;
    uint64_t syscall_limit;
    uint64_t output_limit;
    uint64_t wait_pid;
    uint64_t wait_status_ptr;
    uint64_t wait_flags;
    uint64_t wait_status_value;
    uint64_t sleep_until;
    int64_t exit_status;
    unsigned stack_slot;
    uint8_t reap_claimed;
    uint8_t state;

    char con_buf[CONSOLE_LINE_MAX];
    unsigned con_len;
    unsigned con_ready;

    uint8_t key_ready;
    uint8_t key_code;
    uint8_t key_pressed;
    uint16_t key_mods;
    uint64_t key_deadline;

    int8_t cap_slot;
    uint8_t cap_pad;

    uint64_t pipe_pid;
    uint64_t pipe_deadline;

    struct knx_fd fds[KNX_FD_MAX];
};
static struct task tasks[MAX_TASKS];

#define EXEC_STAGE_MAX (4u * 1024u * 1024u)
static uint8_t *exec_stage = 0;
static int exec_stage_prepare(void) {
    if (exec_stage)
        return 0;
    uint64_t pages = EXEC_STAGE_MAX / 4096;
    uint8_t *base = knx_alloc_contiguous_pages((size_t)pages);
    if (!base)
        return -12;
    exec_stage = base;
    return 0;
}

static uint64_t idle_stacks[MAX_CPUS][512] __attribute__((aligned(16)));
static uint64_t idle_frames[MAX_CPUS][KNX_FRAME_WORDS];
static int idle_frame_ready[MAX_CPUS];

static void kernel_idle(void)
{
    for (;;)
        knx_halt();
}

static void init_idle_frame(unsigned cpu)
{
    if (cpu >= MAX_CPUS)
        cpu = 0;
    if (idle_frame_ready[cpu])
        return;
    for (unsigned i = 0; i < KNX_FRAME_WORDS; ++i)
        idle_frames[cpu][i] = 0;
    idle_frames[cpu][KNX_FRAME_VECTOR] = 0;
    idle_frames[cpu][KNX_FRAME_ERROR] = 0;
    idle_frames[cpu][KNX_FRAME_PC] = (uint64_t)(uintptr_t)kernel_idle;
    idle_frames[cpu][KNX_FRAME_CS] = 0x08;
    idle_frames[cpu][KNX_FRAME_FLAGS] = 0x3c5;
    idle_frames[cpu][KNX_FRAME_SP] = (uint64_t)(uintptr_t)(idle_stacks[cpu] + 512);
    idle_frame_ready[cpu] = 1;
}
static unsigned current_task[MAX_CPUS];

#define CAPTURE_BUF_SIZE 4096
#define MAX_CAPTURES 8
static char capture_buf[MAX_CAPTURES][CAPTURE_BUF_SIZE];
static unsigned capture_len[MAX_CAPTURES];
static int capture_used[MAX_CAPTURES];

static int capture_alloc(void) {
    for (int i = 0; i < MAX_CAPTURES; i++) {
        if (!capture_used[i]) {
            capture_used[i] = 1;
            capture_len[i] = 0;
            capture_buf[i][0] = 0;
            return i;
        }
    }
    return -1;
}

static void capture_free(int slot) {
    if (slot >= 0 && slot < MAX_CAPTURES) {
        capture_used[slot] = 0;
        capture_len[slot] = 0;
    }
}

int knx_task_cap_slot(void) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS) cpu = 0;
    unsigned idx = current_task[cpu];
    if (idx >= MAX_TASKS) return -1;

    if (tasks[idx].state != TASK_RUNNING) return -1;
    return tasks[idx].cap_slot;
}

char *knx_capture_buf(int slot) {
    if (slot < 0 || slot >= MAX_CAPTURES) return 0;
    return capture_buf[slot];
}

unsigned knx_capture_len(int slot) {
    if (slot < 0 || slot >= MAX_CAPTURES) return 0;
    return capture_len[slot];
}

void knx_capture_putc(int slot, char c) {
    if (slot < 0 || slot >= MAX_CAPTURES) return;
    if (capture_len[slot] < CAPTURE_BUF_SIZE - 1) {
        capture_buf[slot][capture_len[slot]++] = c;
        capture_buf[slot][capture_len[slot]] = 0;
    }
}

static void *active_spaces[MAX_CPUS];
static volatile unsigned task_lock;
static volatile unsigned task_count;
static uint64_t next_pid = 2;
static uint64_t next_tid = 3;
static volatile uint64_t syscall_calls;
static volatile uint64_t syscall_audit[40];
static volatile uint32_t selftest_status;

static struct task *console_waiter;

static struct task *console_owner;

static int console_swallow_lf;

static struct task *key_waiter;

int knx_console_owned(void) {
    return console_waiter != 0 || console_owner != 0;
}

static void console_deliver_line(struct task *w) {
    uint64_t max = w->frame[8];
    uint64_t buf = w->frame[9];
    uint64_t len = w->con_len;
    if (!buf || !max)
        len = 0;
    else if (len >= max)
        len = max - 1;
    uint64_t saved_root = knx_vm_current_root();
    if (w->address_space)
        knx_vm_switch(w->address_space);
    if (buf && max) {
        char *out = (char *)(uintptr_t)buf;
        for (uint64_t i = 0; i < len; ++i)
            out[i] = w->con_buf[i];
        out[len] = 0;
    }
    knx_vm_switch((void *)(uintptr_t)saved_root);
    w->con_len = 0;
    w->con_ready = 0;
    w->frame[14] = len;
}

static void console_pump(void) {
    struct task *w = console_waiter;
    if (!w || w->state != TASK_BLOCKED)
        return;
    for (;;) {

        char raw = knx_serial_read_raw();
        int from_serial = raw != 0;
        char c = from_serial ? raw : knx_keyboard_read();
        if (!c)
            return;
        if (console_swallow_lf) {
            console_swallow_lf = 0;
            if (from_serial && raw == '\n')
                continue;
        }
        if (c == '\r' || c == '\n') {
            console_swallow_lf = (from_serial && raw == '\r');
            console_deliver_line(w);
            console_waiter = 0;
            knx_putc('\n');
            w->state = TASK_RUNNABLE;
            return;
        }
        if (c == 0x04) {
            if (!w->con_len) {
                console_deliver_line(w);
                console_waiter = 0;
                knx_puts("^D\n");
                w->state = TASK_RUNNABLE;
                return;
            }
            continue;
        }
        if (c == '\b' || c == 0x7f) {
            if (w->con_len) {
                --w->con_len;
                knx_puts("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && w->con_len < CONSOLE_LINE_MAX - 1) {
            w->con_buf[w->con_len++] = c;
            knx_putc(c);
        }
    }
}

static void key_deliver_event(struct task *w) {
    uint64_t out = w->frame[9];
    uint64_t saved_root = knx_vm_current_root();
    if (w->address_space)
        knx_vm_switch(w->address_space);
    if (out) {
        struct knx_key_event *ev = (struct knx_key_event *)(uintptr_t)out;
        ev->keycode = w->key_code;
        ev->pressed = w->key_pressed;
        ev->modifiers = w->key_mods;
    }
    knx_vm_switch((void *)(uintptr_t)saved_root);
    w->frame[14] = 0;
}

static void key_pump(void) {
    struct task *w = key_waiter;
    if (!w || w->state != TASK_BLOCKED)
        return;

    if (w->key_deadline && knx_ticks() >= w->key_deadline) {
        w->key_deadline = 0;
        w->frame[14] = (uint64_t)(int64_t)-62;
        key_waiter = 0;
        w->state = TASK_RUNNABLE;
        return;
    }
    if (w->key_ready)
        return;
    uint8_t code = 0, pressed = 0;
    uint16_t mods = 0;
    if (!knx_key_event_read(&code, &pressed, &mods))
        return;
    w->key_code = code;
    w->key_pressed = pressed;
    w->key_mods = mods;
    w->key_ready = 1;
    key_deliver_event(w);

    w->key_ready = 0;
    key_waiter = 0;
    w->state = TASK_RUNNABLE;
}

static struct knx_fb_info fb_info;
static int fb_ready;

void knx_fb_get_size(uint32_t *width, uint32_t *height) {
    if (width)
        *width = fb_ready ? fb_info.width : 0;
    if (height)
        *height = fb_ready ? fb_info.height : 0;
}

void knx_fb_set_info(const knx_boot_info *info) {
    if (!info || !info->framebuffer_base || !info->framebuffer_width ||
        !info->framebuffer_height || !info->pixels_per_scanline) {
        fb_ready = 0;
        return;
    }
    fb_info.addr = info->framebuffer_base;
    fb_info.width = info->framebuffer_width;
    fb_info.height = info->framebuffer_height;
    fb_info.pitch = (uint64_t)info->pixels_per_scanline * 4;
    fb_info.size = fb_info.pitch * info->framebuffer_height;
    fb_info.format = info->pixel_format;
    for (unsigned i = 0; i < 4; ++i)
        fb_info.masks[i] = info->pixel_masks[i];
    fb_ready = fb_info.size && fb_info.addr + fb_info.size > fb_info.addr;

    if (fb_ready && knx_vm_framebuffer_init(fb_info.addr, fb_info.size))
        fb_ready = 0;
}

#define SELFTEST_LIFECYCLE_PASS 0x01u
#define SELFTEST_LIFECYCLE_FAIL 0x02u
#define SELFTEST_EXEC_PASS 0x04u
#define SELFTEST_EXEC_FAIL 0x08u
#define SELFTEST_INIT_PASS 0x10u
#define SELFTEST_INIT_FAIL 0x20u

/* Exception frames live on a task's kernel stack and are 40 words wide.
 * The numeric indices used by the scheduler/syscall dispatcher map onto the
 * KNX_FRAME_* definitions shared with kernel/interrupts.S. */
#define KNX_INITIAL_FRAME_WORDS KNX_FRAME_WORDS
#define KNX_INITIAL_FRAME_BYTES (KNX_INITIAL_FRAME_WORDS * sizeof(uint64_t))

extern char __user_text_start, __user_text_end;

static void init_frame(unsigned slot, unsigned user_stack_slot, void (*entry)(void)) {
    uintptr_t top = (uintptr_t)&kernel_stacks[slot][sizeof(kernel_stacks[slot])];
    stack_arm(slot);
    uint64_t *f = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i)
        f[i] = 0;
    f[KNX_FRAME_PC] = (uint64_t)(uintptr_t)entry;
    f[KNX_FRAME_CS] = 0x1b;
    f[KNX_FRAME_FLAGS] = 0;

    f[KNX_FRAME_SP] = USER_STACK_VIRTUAL_BASE + ((uint64_t)user_stack_slot + 1) * USER_STACK_SIZE;
    tasks[slot].frame = f;
    tasks[slot].stack_slot = user_stack_slot;
}

static void lock_tasks(void) {
    while (__sync_lock_test_and_set(&task_lock, 1))
        knx_pause();
}

static void unlock_tasks(void) {
    __sync_lock_release(&task_lock);
}

static volatile char selftest_last_marker;
static volatile uint64_t selftest_last_pid;

static void wake_gated_init(void) {
    if (tasks[1].state == TASK_BLOCKED)
        __atomic_store_n(&tasks[1].state, TASK_RUNNABLE, __ATOMIC_RELEASE);
}

static void report_selftest_byte(char marker, uint64_t pid) {
    uint32_t bit = marker == 'P' ? SELFTEST_LIFECYCLE_PASS
                  : marker == 'F' ? SELFTEST_LIFECYCLE_FAIL
                  : marker == 'E' ? SELFTEST_EXEC_PASS
                  : marker == 'e' ? SELFTEST_EXEC_FAIL
                  : marker == 'I' ? SELFTEST_INIT_PASS
                  : marker == 'i' ? SELFTEST_INIT_FAIL
                                  : 0;
    if (bit) {
        selftest_last_marker = marker;
        selftest_last_pid = pid;
        __sync_fetch_and_or(&selftest_status, bit);
    }
}

static inline uint64_t selftest_tsc(void) {
    return knx_read_tsc();
}

static volatile uint64_t schedule_call_count;
static volatile uint64_t schedule_user_switch_count;

void knx_task_selftest_wait(void) {
    uint64_t tick_deadline = knx_ticks() + 500;
    uint64_t init_wait_start = knx_ticks();
    uint64_t tsc_start = selftest_tsc();
    uint32_t status;

    for (;;) {
        status = __atomic_load_n(&selftest_status, __ATOMIC_ACQUIRE);

        if (status & (SELFTEST_LIFECYCLE_PASS | SELFTEST_LIFECYCLE_FAIL))
            wake_gated_init();
        else if ((int64_t)(knx_ticks() - init_wait_start) >= 12)
            wake_gated_init();
        if ((status & (SELFTEST_LIFECYCLE_PASS | SELFTEST_LIFECYCLE_FAIL)) &&
            (status & (SELFTEST_EXEC_PASS | SELFTEST_EXEC_FAIL)) &&
            (status & (SELFTEST_INIT_PASS | SELFTEST_INIT_FAIL)))
            break;
        if ((int64_t)(knx_ticks() - tick_deadline) >= 0)
            break;
        if (selftest_tsc() - tsc_start > 40000000000ULL)
            break;
        knx_halt();
    }

    wake_gated_init();
    status = __atomic_load_n(&selftest_status, __ATOMIC_ACQUIRE);

    uint64_t summary_flags = knx_irq_flags_save();
    knx_puts("Kernel checks: PID 1 init ");
    knx_puts(status & SELFTEST_INIT_PASS ? "PASS" :
             status & SELFTEST_INIT_FAIL ? "FAIL" : "TIMEOUT");
    knx_puts(" | lifecycle ");
    knx_puts(status & SELFTEST_LIFECYCLE_PASS ? "PASS" :
             status & SELFTEST_LIFECYCLE_FAIL ? "FAIL" : "TIMEOUT");
    knx_puts(" | ELF exec ");
    knx_puts(status & SELFTEST_EXEC_PASS ? "PASS" :
             status & SELFTEST_EXEC_FAIL ? "FAIL" : "TIMEOUT");
    knx_puts(" (status=0x");
    knx_print_u64(status);
    knx_puts(" last=");
    knx_putc(selftest_last_marker ? selftest_last_marker : '-');
    knx_puts(" pid=");
    knx_print_u64(selftest_last_pid);
    knx_puts(" ticks=");
    knx_print_u64(knx_ticks());
    knx_puts(" cpu=");
    knx_print_u64(knx_smp_cpu_index());
    knx_puts(")\n");
    knx_irq_flags_restore(summary_flags);
    if (!status) {

        knx_puts("DBG no-marker count=");
        knx_print_u64(knx_task_count());
        knx_puts(" cur0=");
        knx_print_u64(current_task[0]);
        knx_puts(" sched=");
        knx_print_u64(schedule_call_count);
        knx_puts(" user=");
        knx_print_u64(schedule_user_switch_count);
        knx_puts(" states=");
        for (unsigned i = 0; i < 5; ++i) {
            knx_print_u64(tasks[i].state);
            knx_putc(i < 4 ? ',' : '\n');
        }
    }
    if (status & SELFTEST_INIT_PASS)
        knx_log("[TEST]", "stage3 init PASS");
    else
        knx_log("[TEST]", "stage3 init FAIL");
}

static void reset_task_slot(unsigned slot) {

    if (console_waiter == &tasks[slot])
        console_waiter = 0;
    if (key_waiter == &tasks[slot])
        key_waiter = 0;
    if (console_owner == &tasks[slot])
        console_owner = 0;
    uint8_t *bytes = (uint8_t *)&tasks[slot];
    for (size_t i = 0; i < sizeof(tasks[slot]); ++i)
        bytes[i] = 0;

    tasks[slot].cap_slot = -1;
    stack_arm(slot);
}

static int address_space_referenced(void *space) {
    if (!space)
        return 0;
    for (unsigned i = 1; i < MAX_TASKS; ++i)
        if (tasks[i].state != TASK_UNUSED && tasks[i].address_space == space)
            return 1;
    for (unsigned cpu = 0; cpu < MAX_CPUS; ++cpu)
        if (active_spaces[cpu] == space)
            return 1;
    return 0;
}

static int address_space_referenced_outside_process(void *space, uint64_t pid) {
    if (!space)
        return 0;
    for (unsigned i = 1; i < MAX_TASKS; ++i)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid != pid &&
            tasks[i].address_space == space)
            return 1;
    for (unsigned cpu = 0; cpu < MAX_CPUS; ++cpu)
        if (active_spaces[cpu] == space)
            return 1;
    return 0;
}

static int process_on_cpu(uint64_t pid) {
    for (unsigned cpu = 0; cpu < MAX_CPUS; ++cpu) {
        unsigned index = current_task[cpu];
        if (index < MAX_TASKS && tasks[index].pid == pid)
            return 1;
    }
    return 0;
}

static void reap_claimed_children(void) {
    for (;;) {
        uint64_t pid = 0;
        void *space = 0;
        lock_tasks();
        for (unsigned i = 1; i < MAX_TASKS; ++i) {
            struct task *child = &tasks[i];
            if (child->state != TASK_ZOMBIE || !child->reap_claimed ||
                process_on_cpu(child->pid) ||
                address_space_referenced_outside_process(child->address_space, child->pid))
                continue;
            pid = child->pid;
            space = child->address_space;
            for (unsigned j = 1; j < MAX_TASKS; ++j)
                if (tasks[j].pid == pid && tasks[j].parent_pid == child->parent_pid)
                    tasks[j].state = TASK_UNUSED;
            break;
        }
        unlock_tasks();
        if (!pid)
            return;
        if (space)
            knx_vm_destroy_user(space);
    }
}

static int task_live(const struct task *task) {
    return task->state == TASK_RUNNABLE || task->state == TASK_RUNNING ||
           task->state == TASK_BLOCKED || task->state == TASK_EXEC_PENDING;
}

static int process_has_live_thread(uint64_t pid) {
    for (unsigned i = 1; i < MAX_TASKS; ++i)
        if (tasks[i].pid == pid && task_live(&tasks[i]))
            return 1;
    return 0;
}

static void wake_waiters(uint64_t child_pid) {
    int claimed = 0;

    for (unsigned pass = 0; pass < 2 && !claimed; ++pass) {
        for (unsigned i = 1; i < MAX_TASKS; ++i) {
            struct task *waiter = &tasks[i];
            int exact = (waiter->wait_pid == child_pid);
            if (waiter->state != TASK_BLOCKED)
                continue;
            if (waiter->wait_pid != UINT64_MAX && !exact)
                continue;
            if ((pass == 0 && !exact) || (pass == 1 && exact))
                continue;
            for (unsigned c = 1; c < MAX_TASKS; ++c) {
                struct task *child = &tasks[c];
                if (child->pid != child_pid || child->state != TASK_ZOMBIE ||
                    child->parent_pid != waiter->pid)
                    continue;
                if (!claimed && !child->reap_claimed) {
                    waiter->wait_status_value = (uint32_t)child->exit_status;
                    waiter->frame[14] = child_pid;
                    child->reap_claimed = 1;
                    claimed = 1;
                } else {
                    waiter->frame[14] = (uint64_t)-10;
                }
                waiter->state = TASK_RUNNABLE;
                waiter->wait_pid = 0;
                break;
            }
        }
    }

    if (!claimed) {
        for (unsigned i = 1; i < MAX_TASKS; ++i) {
            struct task *waiter = &tasks[i];
            if (waiter->state != TASK_BLOCKED)
                continue;
            if ((waiter->wait_pid != UINT64_MAX && waiter->wait_pid != child_pid))
                continue;
            waiter->frame[14] = (uint64_t)-10;
            waiter->state = TASK_RUNNABLE;
            waiter->wait_pid = 0;
        }
    }
}

static int reap_process(uint64_t parent_pid, uint64_t child_pid, uint64_t status_ptr,
                        int nohang) {
    int found_child = 0;
    for (unsigned i = 1; i < MAX_TASKS; ++i) {
        struct task *child = &tasks[i];
        if (child->state == TASK_UNUSED || child->reap_claimed)
            continue;
        if (child->parent_pid != parent_pid || (child_pid != UINT64_MAX && child->pid != child_pid))
            continue;
        found_child = 1;
        if (child->state != TASK_ZOMBIE)
            continue;
        uint64_t pid = child->pid;
        int64_t status = child->exit_status;
        void *space = child->address_space;
        int running_on_cpu = 0;
        for (unsigned cpu = 0; cpu < MAX_CPUS; ++cpu)
            if ((current_task[cpu] < MAX_TASKS &&
                 tasks[current_task[cpu]].pid == child_pid) ||
                (space && active_spaces[cpu] == space))
                running_on_cpu = 1;
        if (running_on_cpu)
            return nohang ? 0 : -2;
        if (status_ptr)
            *(int32_t *)(uintptr_t)status_ptr = (int32_t)status;
        for (unsigned j = 1; j < MAX_TASKS; ++j)
            if (tasks[j].pid == pid && tasks[j].parent_pid == parent_pid)
                tasks[j].state = TASK_UNUSED;
        if (space)
            knx_vm_destroy_user(space);
        return (int)pid;
    }
    if (!found_child)
        return -10;
    return nohang ? 0 : -2;
}

void knx_task_cpu_online(unsigned cpu) {
    if (cpu < MAX_CPUS) {
        current_task[cpu] = MAX_TASKS;
        active_spaces[cpu] = 0;
    }
}

int knx_task_is_user_context(void) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;
    unsigned index = current_task[cpu];
    if (index == 0 || index >= MAX_TASKS)
        return 0;
    return tasks[index].state == TASK_RUNNING;
}

void knx_task_debug_current(void) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;
    unsigned index = current_task[cpu];
    knx_puts(" cur=");
    knx_print_u64(index);
    if (index < MAX_TASKS) {
        knx_puts(" pid=");
        knx_print_u64(tasks[index].pid);
        knx_puts(" st=");
        knx_print_u64(tasks[index].state);
    }
    knx_puts("\n");
}

void knx_task_kill_current(void) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;
    unsigned index = current_task[cpu];
    if (index == 0 || index >= MAX_TASKS)
        return;
    struct task *task = &tasks[index];
    knx_puts("DBG kill task ");
    knx_print_u64(index);
    knx_puts(" pid ");
    knx_print_u64(task->pid);
    knx_puts("\n");
    lock_tasks();
    if (console_waiter == task) {
        console_waiter = 0;
        task->con_ready = 0;
        task->con_len = 0;
    }
    if (key_waiter == task) {
        key_waiter = 0;
        task->key_ready = 0;
        task->key_deadline = 0;
    }
    if (console_owner == task)
        console_owner = 0;
    task->exit_status = 139;
    task->state = TASK_EXITED;
    --task_count;
    if (!process_has_live_thread(task->pid)) {
        task->state = TASK_ZOMBIE;
        wake_waiters(task->pid);
    }
    unlock_tasks();
}

__attribute__((section(".usertext"), noinline)) static void user_exec_test_build_and_run(void) {
    volatile uint8_t image[512];
    volatile uint8_t *b = image;
    for (unsigned i = 0; i < 512; ++i)
        b[i] = 0;
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1; b[7] = 0;
    b[16] = 2;
    b[17] = 0;
    b[18] = 183;
    b[19] = 0;
    b[20] = 1;
    b[21] = 0; b[22] = 0; b[23] = 0;
    b[24] = 0; b[25] = 0; b[26] = 0x40; b[27] = 0;
    b[28] = 0; b[29] = 0; b[30] = 0; b[31] = 0;
    b[32] = 64;
    b[33] = 0; b[34] = 0; b[35] = 0; b[36] = 0;
    b[40] = 0; b[41] = 0; b[42] = 0; b[43] = 0;
    b[48] = 0; b[49] = 0; b[50] = 0; b[51] = 0;
    b[52] = 64;
    b[54] = 56;
    b[56] = 1;
    b[57] = 0;
    b[58] = 0; b[59] = 0; b[60] = 0; b[61] = 0; b[62] = 0; b[63] = 0;
    b[64] = 1;
    b[65] = 0; b[66] = 0; b[67] = 0;
    b[68] = 5;
    b[69] = 0; b[70] = 0; b[71] = 0;
    b[72] = 128;
    b[73] = 0; b[74] = 0; b[75] = 0; b[76] = 0; b[77] = 0; b[78] = 0; b[79] = 0;
    b[80] = 0; b[81] = 0; b[82] = 0x40;
    b[83] = 0; b[84] = 0; b[85] = 0;
    b[86] = 0; b[87] = 0; b[88] = 0x40;
    b[89] = 0; b[90] = 0; b[91] = 0;
    b[92] = 1; b[93] = 1;
    b[94] = 0; b[95] = 0;
    b[96] = 1; b[97] = 1;
    b[98] = 0; b[99] = 0;
    b[100] = 0; b[101] = 0; b[102] = 0; b[103] = 0;
    b[104] = 0; b[105] = 16;
    b[106] = 0; b[107] = 0;
    b[108] = 0; b[109] = 0; b[110] = 0; b[111] = 0;

    /* AArch64 code: write the 'E' marker at 0x400100, then exit(7).
     * KNX's write syscall takes (buffer, length) in x0/x1, so the generic
     * fd/buffer/len setup is shuffled into that order before the SVC. */
    static const uint32_t elf_code[] = {
        0xd2800028u, /* mov  x8, #1               write       */
        0xd2800020u, /* mov  x0, #1               fd          */
        0xd2802001u, /* mov  x1, #0x100                       */
        0xf2a08021u, /* movk x1, #0x400, lsl #16  buffer 0x400100 */
        0xd2800042u, /* mov  x2, #1               length      */
        0xaa0103e0u, /* mov  x0, x1               buffer -> arg0 */
        0xaa0203e1u, /* mov  x1, x2               length -> arg1 */
        0xd4000001u, /* svc  #0                               */
        0xd28000c8u, /* mov  x8, #6               exit        */
        0xd28000e0u, /* mov  x0, #7               status      */
        0xd4000001u, /* svc  #0                               */
        0xd503305fu, /* wfi                                   */
    };
    for (unsigned i = 0; i < sizeof(elf_code) / sizeof(elf_code[0]); ++i) {
        unsigned off = 128 + i * 4;
        uint32_t word = elf_code[i];
        b[off + 0] = (uint8_t)(word & 0xffu);
        b[off + 1] = (uint8_t)((word >> 8) & 0xffu);
        b[off + 2] = (uint8_t)((word >> 16) & 0xffu);
        b[off + 3] = (uint8_t)((word >> 24) & 0xffu);
    }
    b[384] = 'E';
    if (knx_sys_exec((const void *)image, sizeof(image)) < 0) {
        char marker = 'e';
        (void)knx_sys_write(&marker, 1);
    }
}

void knx_start_user_tasks(void) {
    uintptr_t start = (uintptr_t)&__user_text_start;
    uintptr_t end = (uintptr_t)&__user_text_end;
    if (end <= start || end - start > 0x100000ULL) {
        knx_puts("User task region invalid; ring 3 disabled.\n");
        return;
    }

    for (unsigned i = 0; i < MAX_TASKS; ++i)
        tasks[i].cap_slot = -1;
    tasks[0].state = TASK_RUNNING;
    tasks[0].pid = 0;
    tasks[0].tid = 0;
    tasks[0].address_space = 0;
    for (unsigned i = 1; i <= 4; ++i) {

        tasks[i].state = i == 1 ? TASK_BLOCKED : TASK_RUNNABLE;
        tasks[i].pid = 1;
        tasks[i].parent_pid = 0;
        tasks[i].tid = i;
        tasks[i].address_space = 0;
        tasks[i].syscall_limit = PROCESS_MAX_SYSCALLS;
        tasks[i].output_limit = PROCESS_MAX_OUTPUT;
        tasks[i].mapped_bytes = 0;
        init_frame(i, i, i == 1 ? user_init : i == 2 ? user_worker_a : user_worker_b);
    }
    current_task[0] = 0;
    task_count = 5;
    knx_log("[TASK]", "Four Ring 3 threads started in process 1");
}

static unsigned find_runnable(unsigned from, unsigned cpu) {
    for (unsigned step = 1; step <= MAX_TASKS; ++step) {
        unsigned i = (from + step) % MAX_TASKS;
        if (i == 0 && cpu != 0)
            continue;
        if (tasks[i].state == TASK_RUNNABLE)
            return i;
    }
    return MAX_TASKS;
}

static void wake_sleepers(void) {
    uint64_t now = knx_ticks();
    for (unsigned i = 1; i < MAX_TASKS; ++i)
        if (tasks[i].state == TASK_SLEEPING && tasks[i].sleep_until <= now)
            tasks[i].state = TASK_RUNNABLE;
}

static void dbg_dump_tasks(const char *why) {
    static uint64_t last;
    uint64_t now = knx_ticks();
    if (now - last < 50)
        return;
    last = now;
    uint64_t root = knx_vm_current_root();
    knx_puts("DBG ");
    knx_puts(why);
    knx_puts(" cur=");
    knx_print_u64(current_task[0]);
    knx_puts(" root=");
    knx_print_u64(root);
    knx_puts(" as0=");
    knx_print_u64((uint64_t)(uintptr_t)active_spaces[0]);
    knx_puts(" n=");
    knx_print_u64(task_count);
    knx_puts("\n");
    for (unsigned i = 0; i < MAX_TASKS; ++i) {
        knx_puts("  [");
        knx_print_u64(i);
        knx_puts(": st=");
        knx_print_u64(tasks[i].state);
        knx_puts(" pid=");
        knx_print_u64(tasks[i].pid);
        knx_puts(" sp=");
        knx_print_u64((uint64_t)(uintptr_t)tasks[i].address_space);
        knx_puts("]\n");
    }
}

static void dbg_check_task_table(void) {
    for (unsigned i = 1; i < MAX_TASKS; ++i) {
        if (tasks[i].state <= TASK_SLEEPING && tasks[i].pid <= 0x10000)
            continue;
        knx_puts("DBG task table corrupt slot=");
        knx_print_u64(i);
        knx_puts(" st=");
        knx_print_u64(tasks[i].state);
        knx_puts(" pid=");
        knx_print_u64(tasks[i].pid);
        knx_puts(" sp=");
        knx_print_u64((uint64_t)(uintptr_t)tasks[i].address_space);
        knx_puts(" frame=");
        knx_print_u64((uint64_t)(uintptr_t)tasks[i].frame);
        knx_puts("\nDBG stacks=");
        knx_print_u64((uint64_t)(uintptr_t)kernel_stacks);
        knx_puts(" tasks=");
        knx_print_u64((uint64_t)(uintptr_t)tasks);
        knx_puts(" exec_stage=");
        knx_print_u64((uint64_t)(uintptr_t)exec_stage);
        knx_puts("\n");
        return;
    }
}

uint64_t *knx_schedule_locked(uint64_t *frame, int may_skip) {
    (void)may_skip;
    __sync_fetch_and_add(&schedule_call_count, 1);
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;

    console_pump();

    key_pump();
    lock_tasks();
    dbg_check_task_table();
    wake_sleepers();
    unsigned previous = current_task[cpu];
    void *retired_space = 0;
    void *exited_thread_space = 0;
    uint64_t exited_pid = 0;
    if (previous < MAX_TASKS &&
        (tasks[previous].state == TASK_RUNNING || tasks[previous].state == TASK_SLEEPING)) {

        tasks[previous].frame = frame;
        if (tasks[previous].state == TASK_RUNNING)
            tasks[previous].state = TASK_RUNNABLE;
        ++tasks[previous].cpu_ticks;
    }
    if (previous < MAX_TASKS && tasks[previous].state == TASK_EXEC_PENDING) {
        retired_space = tasks[previous].retired_space;
        tasks[previous].retired_space = 0;
        tasks[previous].state = TASK_RUNNABLE;
    }
    if (previous > 0 && previous < MAX_TASKS && tasks[previous].state == TASK_ZOMBIE)
        exited_pid = tasks[previous].pid;
    unsigned exited_thread = MAX_TASKS;
    if (previous > 0 && previous < MAX_TASKS && tasks[previous].state == TASK_EXITED) {
        exited_thread = previous;
        exited_thread_space = tasks[previous].address_space;
    }
    if (exited_pid)
        wake_waiters(exited_pid);
    unsigned next = find_runnable(previous < MAX_TASKS ? previous : MAX_TASKS - 1, cpu);
    if (next == MAX_TASKS) {
        if (previous < MAX_TASKS && tasks[previous].state == TASK_RUNNABLE) {
            next = previous;
        } else {
            dbg_dump_tasks("idle");

            if (previous >= MAX_TASKS ||
                (tasks[previous].state != TASK_EXITED &&
                 tasks[previous].state != TASK_ZOMBIE))
                current_task[cpu] = MAX_TASKS;
            unlock_tasks();
            if (previous < MAX_TASKS &&
                (tasks[previous].state == TASK_EXITED ||
                 tasks[previous].state == TASK_ZOMBIE)) {

                init_idle_frame(cpu);
                knx_vm_switch(0);
                return idle_frames[cpu];
            }
            return frame;
        }
    }
    tasks[next].state = TASK_RUNNING;
    current_task[cpu] = next;
    if (next > 0)
        __sync_fetch_and_add(&schedule_user_switch_count, 1);
    if (next < MAX_TASKS && !stack_intact(next)) {
        dbg_report_stack_smash(next);
    }
    if (exited_thread < MAX_TASKS)
        tasks[exited_thread].state = TASK_UNUSED;
    uint64_t *next_frame = tasks[next].frame;
    void *space = tasks[next].address_space;
    unlock_tasks();
    knx_vm_switch(space);
    if (next < MAX_TASKS && tasks[next].wait_status_ptr) {
        *(int32_t *)(uintptr_t)tasks[next].wait_status_ptr =
            (int32_t)tasks[next].wait_status_value;
        tasks[next].wait_status_ptr = 0;
    }
    if (next)
        knx_set_kernel_stack((uintptr_t)&kernel_stacks[next][sizeof(kernel_stacks[0])]);
    lock_tasks();
    active_spaces[cpu] = space;
    if (retired_space) {
        if (!address_space_referenced(retired_space)) {
            unlock_tasks();
            knx_vm_destroy_user(retired_space);
            lock_tasks();
        }
    }
    if (exited_thread_space) {
        if (!address_space_referenced(exited_thread_space)) {
            unlock_tasks();
            knx_vm_destroy_user(exited_thread_space);
            lock_tasks();
        }
    }
    unlock_tasks();
    reap_claimed_children();
    return next_frame;
}

uint64_t *knx_schedule(uint64_t *frame) {

    if (!knx_smp_is_bsp())
        return frame;
    return knx_schedule_locked(frame, 0);
}

static int user_readable(uint64_t address, uint64_t length) {
    if (!length || length > 0x10000 || address + length < address)
        return 0;
    return knx_vm_user_readable(address, length);
}

static int user_writable(uint64_t address, uint64_t length) {
    if (!length || length > 0x100000 || address + length < address)
        return 0;
    /* No separate writable-range helper exists in the VM API yet; the
     * user-accessible check is the closest available validation. */
    return knx_vm_user_readable(address, length);
}

static int process_fork(uint64_t *frame) {
    unsigned parent_index = current_task[(unsigned)knx_smp_cpu_index()];
    if (parent_index >= MAX_TASKS)
        return -1;
    uint64_t user_rsp = frame[20];
    uint64_t stack_end = USER_STACK_VIRTUAL_BASE + sizeof(user_stacks);
    if (user_rsp <= USER_STACK_VIRTUAL_BASE || user_rsp > stack_end)
        return -14;
    unsigned slot;
    for (slot = 1; slot < MAX_TASKS && tasks[slot].state != TASK_UNUSED; ++slot) {
    }
    if (slot == MAX_TASKS)
        return -11;
    void *space = knx_vm_clone_user();
    if (!space)
        return -12;
    reset_task_slot(slot);
    uintptr_t top = (uintptr_t)&kernel_stacks[slot][sizeof(kernel_stacks[slot])];
    uint64_t *child = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i)
        child[i] = frame[i];
    child[14] = 0;
    tasks[slot].frame = child;
    tasks[slot].address_space = space;
    tasks[slot].pid = next_pid++;
    tasks[slot].parent_pid = tasks[current_task[(unsigned)knx_smp_cpu_index()]].pid;
    tasks[slot].tid = 1;
    tasks[slot].syscall_limit = PROCESS_MAX_SYSCALLS;
    tasks[slot].output_limit = PROCESS_MAX_OUTPUT;
    tasks[slot].mapped_bytes = tasks[parent_index].mapped_bytes;
    tasks[slot].stack_slot = (unsigned)((user_rsp - USER_STACK_VIRTUAL_BASE - 1) / USER_STACK_SIZE);
    tasks[slot].state = 1;
    ++task_count;
    return (int)tasks[slot].pid;
}

static int task_exec(uint64_t image, uint64_t size) {
    unsigned task_index = current_task[(unsigned)knx_smp_cpu_index()];
    struct task *task = &tasks[task_index];
    if (console_waiter == task) {
        console_waiter = 0;
        task->con_ready = 0;
        task->con_len = 0;
    }
    if (key_waiter == task) {
        key_waiter = 0;
        task->key_ready = 0;
        task->key_deadline = 0;
    }
    knx_puts("DBG task_exec image="); knx_print_u64(image); knx_puts(" size="); knx_print_u64(size); knx_puts("\n");
    if (!image || size < 64 || size > 0x200000) {
        knx_puts("DBG task_exec fail: bounds\n");
        return -8;
    }
    if (!user_readable(image, size)) {
        knx_puts("DBG task_exec fail: user_readable\n");
        return -14;
    }
    uint64_t entry = 0, end = 0;
    uint64_t stack_va = USER_STACK_VIRTUAL_BASE + ((uint64_t)task->stack_slot + 1) * USER_STACK_SIZE;
    void *stack_page = knx_alloc_page();
    if (!stack_page) {
        knx_puts("DBG task_exec fail: no page\n");
        return -12;
    }
    uint8_t *stack_bytes = stack_page;
    for (unsigned i = 0; i < 4096; ++i)
        stack_bytes[i] = 0;
    uint64_t stack_frame = (uint64_t)(uintptr_t)stack_page;
    void *space = knx_vm_load_elf((const void *)(uintptr_t)image, size, &end, &entry, stack_va,
                                  stack_frame);
    if (!space) {
        knx_puts("DBG task_exec fail: load_elf\n");
        knx_free_page(stack_page);
        return -8;
    }
    uintptr_t top = (uintptr_t)&kernel_stacks[task_index][sizeof(kernel_stacks[task_index])];
    uint64_t *frame = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i)
        frame[i] = 0;
    frame[15] = 128;
    frame[16] = 0;
    frame[17] = entry;
    frame[18] = 0x1b;
    frame[19] = 0x202;
    frame[20] = stack_va - 8;
    frame[21] = 0x23;
    void *old_space = task->address_space;
    task->frame = (uint64_t *)frame;
    task->address_space = space;
    task->retired_space = old_space;
    task->mapped_bytes = 0;
    task->state = TASK_EXEC_PENDING;
    task->wait_pid = 0;
    task->wait_status_ptr = 0;
    task->wait_flags = 0;
    task->wait_status_value = 0;
    return 0;
}

static int copy_user_string(uint64_t address, char *out, unsigned capacity) {
    if (!address || !capacity)
        return 0;
    for (unsigned i = 0; i < capacity; ++i) {
        if (!user_readable(address + i, 1))
            return 0;
        out[i] = *(const char *)(uintptr_t)(address + i);
        if (!out[i])
            return 1;
    }
    return 0;
}

static int user_string_length(uint64_t address, unsigned limit) {
    for (unsigned i = 0; i < limit; ++i) {
        if (!user_readable(address + i, 1))
            return -1;
        if (!*(const char *)(uintptr_t)(address + i))
            return (int)i;
    }
    return -1;
}

static int copy_user_string_into(uint64_t address, uint8_t *dst, unsigned limit) {
    for (unsigned i = 0; i < limit; ++i) {
        if (!user_readable(address + i, 1))
            return -1;
        uint8_t c = *(const uint8_t *)(uintptr_t)(address + i);
        dst[i] = c;
        if (!c)
            return (int)i;
    }
    return -1;
}

static int read_user_vector_entry(uint64_t vector, unsigned index, uint64_t *out) {
    uint64_t slot = vector + (uint64_t)index * sizeof(uint64_t);
    if (slot < vector || !user_readable(slot, sizeof(uint64_t)))
        return 0;
    *out = *(const uint64_t *)(uintptr_t)slot;
    return 1;
}

static int count_user_vector(uint64_t vector, unsigned *count) {
    *count = 0;
    if (!vector)
        return 1;
    for (unsigned i = 0; i < EXEC_MAX_VECTOR; ++i) {
        uint64_t string = 0;
        if (!read_user_vector_entry(vector, i, &string))
            return 0;
        if (!string) {
            *count = i;
            return 1;
        }
        *count = i + 1;
    }
    return 1;
}

static const char *const exec_path_dirs[] = { "/bin", "/kfs", "/kfs/bin", 0 };

static unsigned exec_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

static void exec_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static int exec_has_slash(const char *s) {
    for (; *s; ++s)
        if (*s == '/') return 1;
    return 0;
}

static int exec_resolve(const char *path, struct knx_vnode **node,
                        struct knx_vfs_stat *stat) {
    char candidate[KNX_VFS_PATH_MAX];
    struct knx_vnode *n = 0;
    struct knx_vfs_stat s = {0};
    unsigned len, i;

    if (!path || !(len = exec_strlen(path)))
        return -2;
    if (exec_has_slash(path)) {
        if (path[0] != '/') {
            if (len + 2 > sizeof(candidate)) return -2;
            candidate[0] = '/';
            exec_strcpy(candidate + 1, path);
            path = candidate;
        }
        if (knx_vfs_resolve(path, &n) || !n || knx_vfs_stat(n, &s) ||
            s.type != KNX_VFS_REGULAR)
            return -2;
        *node = n;
        *stat = s;
        return 0;
    }
    for (i = 0; exec_path_dirs[i]; ++i) {
        unsigned dir_len = exec_strlen(exec_path_dirs[i]);

        if (dir_len + len + 6 > sizeof(candidate)) continue;
        exec_strcpy(candidate, exec_path_dirs[i]);
        candidate[dir_len] = '/';
        exec_strcpy(candidate + dir_len + 1, path);
        if (!knx_vfs_resolve(candidate, &n) && n && !knx_vfs_stat(n, &s) &&
            s.type == KNX_VFS_REGULAR) {
            *node = n;
            *stat = s;
            return 0;
        }
        exec_strcpy(candidate + dir_len + 1 + len, ".elf");
        if (!knx_vfs_resolve(candidate, &n) && n && !knx_vfs_stat(n, &s) &&
            s.type == KNX_VFS_REGULAR) {
            *node = n;
            *stat = s;
            return 0;
        }
    }
    return -2;
}

static int task_execve(uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    char path[KNX_VFS_PATH_MAX];
    unsigned argc = 0, envc = 0;
    if (!copy_user_string(user_path, path, sizeof(path)))
        return -14;
    if (!count_user_vector(user_argv, &argc) || !count_user_vector(user_envp, &envc))
        return -14;
    struct knx_vnode *node = 0;
    struct knx_vfs_stat stat = {0};
    int rc = exec_resolve(path, &node, &stat);
    if (rc) {
        knx_puts("DBG execve fail: resolve rc=");
        knx_print_u64((int64_t)rc);
        knx_putc('\n');
        return rc;
    }

    if (!stat.size || stat.size > EXEC_STAGE_MAX) {
        knx_puts("DBG execve fail: size=");
        knx_print_u64(stat.size);
        knx_putc('\n');
        return -8;
    }
    if (exec_stage_prepare() < 0) {
        knx_puts("DBG execve fail: stage\n");
        return -12;
    }
    uint8_t *image = exec_stage;
    uint64_t done = 0;
    while (done < stat.size) {
        uint64_t take = stat.size - done;
        if (take > 4096)
            take = 4096;
        int64_t got = knx_vfs_read(node, done, image + done, take);
        if (got != (int64_t)take) {
            knx_puts("DBG execve fail: read got=");
            knx_print_u64(got);
            knx_putc('\n');
            return -8;
        }
        done += take;
    }

    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;
    unsigned task_index = current_task[cpu];
    struct task *task = &tasks[task_index];
    uint64_t stack_va = USER_STACK_VIRTUAL_BASE + ((uint64_t)task->stack_slot + 1) * USER_STACK_SIZE;
    uint8_t *stack = knx_alloc_page();
    if (!stack) {
        knx_puts("DBG execve fail: no page\n");

        return -12;
    }
    for (unsigned i = 0; i < 4096; ++i)
        stack[i] = 0;
    uint64_t arg_ptrs[EXEC_MAX_VECTOR], env_ptrs[EXEC_MAX_VECTOR];
    uint64_t sp = 4096;

    for (unsigned i = envc; i > 0; --i) {
        uint64_t string = 0;
        if (!read_user_vector_entry(user_envp, i - 1, &string))
            goto bad_vector;
        int len = user_string_length(string, EXEC_MAX_STRINGS);
        if (len < 0)
            goto bad_vector;
        sp -= (uint64_t)len + 1;
        if (copy_user_string_into(string, stack + sp, EXEC_MAX_STRINGS) < 0)
            goto bad_vector;
        env_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    for (unsigned i = argc; i > 0; --i) {
        uint64_t string = 0;
        if (!read_user_vector_entry(user_argv, i - 1, &string))
            goto bad_vector;
        int len = user_string_length(string, EXEC_MAX_STRINGS);
        if (len < 0)
            goto bad_vector;
        sp -= (uint64_t)len + 1;
        if (copy_user_string_into(string, stack + sp, EXEC_MAX_STRINGS) < 0)
            goto bad_vector;
        arg_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    sp &= ~7ULL;
    uint64_t frame_words = 1 + argc + 1 + envc + 1;
    uint64_t frame_bytes = frame_words * sizeof(uint64_t);
    if (sp < frame_bytes + 8) {
        knx_free_page(stack);

        return -7;
    }
    sp = (sp - frame_bytes) & ~15ULL;
    uint64_t *initial = (uint64_t *)(void *)(stack + sp);
    unsigned n = 0;
    initial[n++] = argc;
    for (unsigned i = 0; i < argc; ++i) initial[n++] = (uint64_t)(uintptr_t)arg_ptrs[i];
    initial[n++] = 0;
    for (unsigned i = 0; i < envc; ++i) initial[n++] = (uint64_t)(uintptr_t)env_ptrs[i];
    initial[n++] = 0;
    uint64_t entry = 0;
    void *space = knx_vm_load_elf(image, stat.size, 0, &entry, stack_va,
                                  (uint64_t)(uintptr_t)stack);
    if (!space) {
        knx_puts("DBG execve fail: load_elf\n");
        knx_free_page(stack);
        return -8;
    }
    uintptr_t top = (uintptr_t)&kernel_stacks[task_index][sizeof(kernel_stacks[task_index])];
    uint64_t *frame = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i) frame[i] = 0;
    frame[15] = 128;
    frame[17] = entry;
    frame[18] = 0x1b;
    frame[19] = 0x202;
    frame[20] = stack_va - 4096 + sp;
    frame[21] = 0x23;
    void *old_space = task->address_space;
    task->frame = frame;
    task->address_space = space;

    task->retired_space = old_space;
    task->state = TASK_EXEC_PENDING;
    task->mapped_bytes = 0;
    return 0;

bad_vector:
    knx_free_page(stack);
    return -14;
}

int knx_kernel_exec(const char *path, const char *const argv[], const char *const envp[])
{
    if (!path || !argv)
        return -22;
    struct knx_vnode *node = 0;
    struct knx_vfs_stat stat = {0};
    int rc = exec_resolve(path, &node, &stat);
    if (rc) return rc;

    if (!stat.size || stat.size > EXEC_STAGE_MAX)
        return -8;

    unsigned slot;
    for (slot = 1; slot < MAX_TASKS && tasks[slot].state != TASK_UNUSED; ++slot) {
    }
    if (slot == MAX_TASKS)
        return -11;

    unsigned stack_slot;
    for (stack_slot = 1; stack_slot < MAX_TASKS; ++stack_slot) {
        int used = 0;
        for (unsigned i = 1; i < MAX_TASKS; ++i)
            if (task_live(&tasks[i]) && tasks[i].stack_slot == stack_slot)
                used = 1;
        if (!used)
            break;
    }
    if (stack_slot == MAX_TASKS)
        return -11;

    uint64_t stack_va = USER_STACK_VIRTUAL_BASE + ((uint64_t)stack_slot + 1) * USER_STACK_SIZE;

    if (stat.size > EXEC_STAGE_MAX)
        return -8;
    if (exec_stage_prepare() < 0)
        return -12;
    uint8_t *image = exec_stage;
    uint64_t done = 0;
    while (done < stat.size) {
        uint64_t take = stat.size - done;
        if (take > 4096)
            take = 4096;
        int64_t got = knx_vfs_read(node, done, image + done, take);
        if (got != (int64_t)take)
            return -8;
        done += take;
    }

    uint8_t *stack = knx_alloc_page();
    if (!stack) {

        return -12;
    }
    for (unsigned i = 0; i < 4096; ++i)
        stack[i] = 0;

    unsigned argc = 0, envc = 0;
    while (argv[argc] && argc < EXEC_MAX_VECTOR) ++argc;
    while (envp && envp[envc] && envc < EXEC_MAX_VECTOR) ++envc;

    uint64_t arg_ptrs[EXEC_MAX_VECTOR], env_ptrs[EXEC_MAX_VECTOR];
    uint64_t sp = 4096;
    for (unsigned i = envc; i > 0; --i) {
        unsigned len = 0;
        while (envp[i - 1][len]) ++len;
        sp -= len + 1;
        for (unsigned j = 0; j <= len; ++j)
            stack[sp + j] = (uint8_t)envp[i - 1][j];
        env_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    for (unsigned i = argc; i > 0; --i) {
        unsigned len = 0;
        while (argv[i - 1][len]) ++len;
        sp -= len + 1;
        for (unsigned j = 0; j <= len; ++j)
            stack[sp + j] = (uint8_t)argv[i - 1][j];
        arg_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    sp &= ~7ULL;
    uint64_t frame_bytes = (1 + argc + 1 + envc + 1) * sizeof(uint64_t);
    if (sp < frame_bytes + 8) {
        knx_free_page(stack);

        return -7;
    }
    sp = (sp - frame_bytes) & ~15ULL;
    uint64_t *initial = (uint64_t *)(void *)(stack + sp);
    unsigned n = 0;
    initial[n++] = argc;
    for (unsigned i = 0; i < argc; ++i)
        initial[n++] = arg_ptrs[i];
    initial[n++] = 0;
    for (unsigned i = 0; i < envc; ++i)
        initial[n++] = env_ptrs[i];
    initial[n++] = 0;

    uint64_t entry = 0;
    void *space = knx_vm_load_elf(image, stat.size, 0, &entry, stack_va,
                                  (uint64_t)(uintptr_t)stack);
    if (!space) {
        knx_free_page(stack);
        return -8;
    }

    lock_tasks();
    reset_task_slot(slot);
    uintptr_t top = (uintptr_t)&kernel_stacks[slot][sizeof(kernel_stacks[slot])];
    uint64_t *frame = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i)
        frame[i] = 0;
    frame[15] = 128;
    frame[17] = entry;
    frame[18] = 0x1b;
    frame[19] = 0x202;
    frame[20] = stack_va - 4096 + sp;
    frame[21] = 0x23;
    tasks[slot].frame = frame;
    tasks[slot].address_space = space;
    tasks[slot].retired_space = 0;
    tasks[slot].pid = next_pid++;
    tasks[slot].parent_pid = 0;
    tasks[slot].tid = 1;
    tasks[slot].syscall_limit = PROCESS_MAX_SYSCALLS;
    tasks[slot].output_limit = PROCESS_MAX_OUTPUT;
    tasks[slot].mapped_bytes = 0;
    tasks[slot].stack_slot = stack_slot;

    tasks[slot].state = TASK_RUNNABLE;
    ++task_count;
    uint64_t new_pid = tasks[slot].pid;
    unlock_tasks();
    return (int)new_pid;
}

static int task_spawn(uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    char path[KNX_VFS_PATH_MAX];
    if (!copy_user_string(user_path, path, sizeof(path)))
        return -14;
    unsigned argc = 0, envc = 0;
    if (!count_user_vector(user_argv, &argc) || !count_user_vector(user_envp, &envc))
        return -14;
    struct knx_vnode *node = 0;
    struct knx_vfs_stat stat = {0};
    int rc = exec_resolve(path, &node, &stat);
    if (rc) return rc;
    if (!stat.size || stat.size > EXEC_STAGE_MAX)
        return -8;

    unsigned slot;
    for (slot = 1; slot < MAX_TASKS && tasks[slot].state != TASK_UNUSED; ++slot) {}
    if (slot == MAX_TASKS)
        return -11;

    unsigned stack_slot;
    for (stack_slot = 1; stack_slot < MAX_TASKS; ++stack_slot) {
        int used = 0;
        for (unsigned i = 1; i < MAX_TASKS; ++i)
            if (task_live(&tasks[i]) && tasks[i].stack_slot == stack_slot)
                used = 1;
        if (!used) break;
    }
    if (stack_slot == MAX_TASKS)
        return -11;

    int cap = capture_alloc();
    if (cap < 0)
        return -12;

    uint64_t stack_va = USER_STACK_VIRTUAL_BASE + ((uint64_t)stack_slot + 1) * USER_STACK_SIZE;

    if (exec_stage_prepare() < 0) {
        capture_free(cap);
        return -12;
    }
    uint8_t *image = exec_stage;
    uint64_t done = 0;
    while (done < stat.size) {
        uint64_t take = stat.size - done;
        if (take > 4096) take = 4096;
        int64_t got = knx_vfs_read(node, done, image + done, take);
        if (got != (int64_t)take) {
            capture_free(cap);
            return -8;
        }
        done += take;
    }

    uint8_t *stack = knx_alloc_page();
    if (!stack) {
        capture_free(cap);
        return -12;
    }
    for (unsigned i = 0; i < 4096; ++i)
        stack[i] = 0;

    uint64_t arg_ptrs[EXEC_MAX_VECTOR], env_ptrs[EXEC_MAX_VECTOR];
    uint64_t sp = 4096;
    for (unsigned i = envc; i > 0; --i) {
        uint64_t string = 0;
        if (!read_user_vector_entry(user_envp, i - 1, &string))
            goto bad;
        int len = user_string_length(string, EXEC_MAX_STRINGS);
        if (len < 0) goto bad;
        sp -= (uint64_t)len + 1;
        if (copy_user_string_into(string, stack + sp, EXEC_MAX_STRINGS) < 0)
            goto bad;
        env_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    for (unsigned i = argc; i > 0; --i) {
        uint64_t string = 0;
        if (!read_user_vector_entry(user_argv, i - 1, &string)) goto bad;
        int len = user_string_length(string, EXEC_MAX_STRINGS);
        if (len < 0) goto bad;
        sp -= (uint64_t)len + 1;
        if (copy_user_string_into(string, stack + sp, EXEC_MAX_STRINGS) < 0)
            goto bad;
        arg_ptrs[i - 1] = stack_va - 4096 + sp;
    }
    sp &= ~7ULL;
    uint64_t frame_words = 1 + argc + 1 + envc + 1;
    uint64_t frame_bytes = frame_words * sizeof(uint64_t);
    if (sp < frame_bytes + 8) {
        knx_free_page(stack);
        capture_free(cap);
        return -7;
    }
    sp = (sp - frame_bytes) & ~15ULL;
    uint64_t *initial = (uint64_t *)(void *)(stack + sp);
    unsigned n = 0;
    initial[n++] = argc;
    for (unsigned i = 0; i < argc; ++i) initial[n++] = arg_ptrs[i];
    initial[n++] = 0;
    for (unsigned i = 0; i < envc; ++i) initial[n++] = env_ptrs[i];
    initial[n++] = 0;

    uint64_t entry = 0;
    void *space = knx_vm_load_elf(image, stat.size, 0, &entry, stack_va,
                                  (uint64_t)(uintptr_t)stack);
    if (!space) {
        knx_free_page(stack);
        capture_free(cap);
        return -8;
    }

    lock_tasks();
    reset_task_slot(slot);
    uintptr_t top = (uintptr_t)&kernel_stacks[slot][sizeof(kernel_stacks[slot])];
    uint64_t *frame = (uint64_t *)(top - KNX_INITIAL_FRAME_BYTES);
    for (unsigned i = 0; i < KNX_INITIAL_FRAME_WORDS; ++i)
        frame[i] = 0;
    frame[15] = 128;
    frame[17] = entry;
    frame[18] = 0x1b;
    frame[19] = 0x202;
    frame[20] = stack_va - 4096 + sp;
    frame[21] = 0x23;
    tasks[slot].frame = frame;
    tasks[slot].address_space = space;
    tasks[slot].retired_space = 0;
    tasks[slot].pid = next_pid++;
    tasks[slot].parent_pid = tasks[current_task[(unsigned)knx_smp_cpu_index()]].pid;
    tasks[slot].tid = 1;
    tasks[slot].syscall_limit = PROCESS_MAX_SYSCALLS;
    tasks[slot].output_limit = PROCESS_MAX_OUTPUT;
    tasks[slot].mapped_bytes = 0;
    tasks[slot].stack_slot = stack_slot;

    tasks[slot].state = TASK_RUNNABLE;
    tasks[slot].cap_slot = (int8_t)cap;
    tasks[slot].pipe_pid = 0;
    tasks[slot].pipe_deadline = 0;
    ++task_count;
    uint64_t new_pid = tasks[slot].pid;
    unlock_tasks();
    return (int)new_pid;

bad:
    knx_free_page(stack);
    capture_free(cap);
    return -14;
}

int knx_syscall(uint64_t *frame) {
    __sync_fetch_and_add(&syscall_calls, 1);
    uint64_t number = frame[14];
    if (number < 40)
        __sync_fetch_and_add(&syscall_audit[number], 1);
    if ((frame[18] & 3) != 3) {
        frame[14] = (uint64_t)-1;
        return 0;
    }
    if (!frame[20] || !user_readable(frame[17], 1) || !user_readable(frame[20] - 1, 1)) {
        frame[14] = (uint64_t)-14;
        return 0;
    }
    uint64_t arg0 = frame[9];
    uint64_t arg1 = frame[8];
    uint64_t arg2 = frame[11];
    uint64_t arg3 = frame[12];
    uint64_t arg4 = frame[7];
    uint64_t arg5 = frame[6];
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPUS)
        cpu = 0;
    unsigned index = current_task[cpu];
    if (index >= MAX_TASKS || tasks[index].state != TASK_RUNNING) {
        frame[14] = (uint64_t)-1;
        return 0;
    }
    struct task *task = &tasks[index];

    if (number != 6 && ++task->syscalls > task->syscall_limit) {
        if (task->pid) {         }
        frame[14] = (uint64_t)-11;
        return 0;
    }
    if (number == 38) {
        knx_warm_reboot();
        return 0;
    }
    if (number == 39) {
        if (!arg0 || !arg1 || !user_writable(arg0, arg1)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        uint32_t cap = arg1 > 0x20000u ? 0x20000u : (uint32_t)arg1;
        frame[14] = knx_flight_read((uint8_t *)(uintptr_t)arg0, cap);
        return 0;
    }
    if (number == 0) {
        uint64_t sleep_ticks = arg0;
        if (sleep_ticks) {
            if (task->cpu_ticks < 2) {            }
            lock_tasks();
            task->sleep_until = knx_ticks() + sleep_ticks;
            task->state = TASK_SLEEPING;
            unlock_tasks();
        }
        return 1;
    }
    if (number == 1) {
        if (!arg1 || arg1 > 4096 || arg1 > task->output_limit - task->output_bytes ||
            !user_readable(arg0, arg1)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        const char *s = (const char *)(uintptr_t)arg0;
        if (task->pid == 1 && arg1 == 1 &&
            (s[0] == 'P' || s[0] == 'F' || s[0] == 'E' || s[0] == 'e' ||
             s[0] == 'I' || s[0] == 'i')) {
            report_selftest_byte(s[0], task->pid);
            task->output_bytes += 1;
            frame[14] = 1;
            return 0;
        }

        uint64_t flags = knx_irq_flags_save();
        for (uint64_t i = 0; i < arg1; ++i)
            knx_putc(s[i]);
        knx_irq_flags_restore(flags);
        task->output_bytes += arg1;
        frame[14] = arg1;
        return 0;
    }
    if (number == 2) {
        frame[14] = task->pid;
        return 0;
    }
    if (number == 3) {
        frame[14] = task->tid;
        return 0;
    }
    if (number == 4) {
        frame[14] = knx_ticks();
        return 0;
    }
    if (number == 5) {
        lock_tasks();
        frame[14] = (uint64_t)(int64_t)process_fork(frame);
        unlock_tasks();
        return 0;
    }
    if (number == 7) {
        uint64_t text_start = (uint64_t)(uintptr_t)&__user_text_start;
        uint64_t text_end = (uint64_t)(uintptr_t)&__user_text_end;
        if (arg0 < text_start || arg0 >= text_end) {
            frame[14] = (uint64_t)-22;
            return 0;
        }
        lock_tasks();
        unsigned slot;
        for (slot = 1; slot < MAX_TASKS && tasks[slot].state != TASK_UNUSED; ++slot) {
        }
        if (slot == MAX_TASKS) {
            unlock_tasks();
            frame[14] = (uint64_t)-11;
            return 0;
        }
        unsigned stack_slot;
        for (stack_slot = 1; stack_slot < MAX_TASKS; ++stack_slot) {
            int used = 0;
            for (unsigned i = 1; i < MAX_TASKS; ++i)
                if (task_live(&tasks[i]) &&
                    tasks[i].pid == task->pid && tasks[i].stack_slot == stack_slot)
                    used = 1;
            if (!used)
                break;
        }
        if (stack_slot == MAX_TASKS) {
            unlock_tasks();
            frame[14] = (uint64_t)-11;
            return 0;
        }
        reset_task_slot(slot);
        uint64_t stack = USER_STACK_VIRTUAL_BASE + (uint64_t)stack_slot * USER_STACK_SIZE;
        volatile uint64_t *words = (volatile uint64_t *)(uintptr_t)stack;
        for (unsigned i = 0; i < USER_STACK_SIZE / sizeof(uint64_t); ++i)
            words[i] = 0;
        tasks[slot].pid = task->pid;
        tasks[slot].parent_pid = task->parent_pid;
        tasks[slot].tid = next_tid++;
        tasks[slot].stack_slot = stack_slot;
        tasks[slot].address_space = task->address_space;
        tasks[slot].syscall_limit = task->syscall_limit;
        tasks[slot].output_limit = task->output_limit;
        tasks[slot].state = 1;
        init_frame(slot, stack_slot, (void (*)(void))(uintptr_t)arg0);
        frame[14] = tasks[slot].tid;
        ++task_count;
        unlock_tasks();
        return 0;
    }
    if (number == 8) {
        uint64_t wanted = (int64_t)arg0 == -1 ? UINT64_MAX : arg0;
        if (arg1 && !user_writable(arg1, sizeof(int32_t))) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        uint64_t options = frame[11];
        if (options & ~1ULL) {
            frame[14] = (uint64_t)-22;
            return 0;
        }
        lock_tasks();
        int result = reap_process(task->pid, wanted, arg1, options & 1);
        if (result == -2) {
            task->frame = frame;
            task->wait_pid = wanted;
            task->wait_status_ptr = arg1;
            task->wait_flags = options;
            task->state = TASK_BLOCKED;
            unlock_tasks();
            return 1;
        }
        frame[14] = (uint64_t)(int64_t)result;
        unlock_tasks();
        return 0;
    }
    if (number == 10) {
        if (!arg0 || arg0 > PROCESS_MAX_MAPPED - task->mapped_bytes) {
            frame[14] = (uint64_t)-12;
            return 0;
        }
        int64_t mapped = knx_vm_map_anonymous(arg0, arg1);
        if (mapped >= 0)
            task->mapped_bytes += (arg0 + 4095ULL) & ~4095ULL;
        frame[14] = (uint64_t)mapped;
        return 0;
    }
    if (number == 11) {
        int unmapped = knx_vm_unmap(arg0, arg1);
        if (!unmapped) {
            uint64_t bytes = (arg1 + 4095ULL) & ~4095ULL;
            task->mapped_bytes = bytes > task->mapped_bytes ? 0 : task->mapped_bytes - bytes;
        }
        frame[14] = (uint64_t)(int64_t)unmapped;
        return 0;
    }
    if (number == 12) {
        lock_tasks();
        frame[14] = (uint64_t)(int64_t)task_exec(arg0, arg1);
        int switched = task->state == TASK_EXEC_PENDING;
        unlock_tasks();
        return switched;
    }
    if (number == 13) {
        lock_tasks();
        frame[14] = (uint64_t)(int64_t)task_execve(arg0, arg1, frame[11]);
        int switched = task->state == TASK_EXEC_PENDING;
        unlock_tasks();
        return switched;
    }
    if (number == 14) {
        char path[KNX_VFS_PATH_MAX];
        if (!arg0 || !copy_user_string(arg0, path, sizeof(path))) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        struct knx_vnode *node = 0;
        struct knx_vfs_stat stat = {0};
        if (knx_vfs_resolve(path, &node) || !node ||
            knx_vfs_stat(node, &stat) || stat.type != KNX_VFS_REGULAR) {
            frame[14] = (uint64_t)-2;
            return 0;
        }
        int fd = -1;
        for (int i = 0; i < KNX_FD_MAX; ++i)
            if (!task->fds[i].open) {
                fd = i;
                break;
            }
        if (fd < 0) {
            frame[14] = (uint64_t)-10;
            return 0;
        }
        task->fds[fd].node = node;
        task->fds[fd].offset = 0;
        task->fds[fd].size = stat.size;
        task->fds[fd].open = 1;
        frame[14] = (uint64_t)fd;
        return 0;
    }
    if (number == 15) {
        if (arg0 >= KNX_FD_MAX || !task->fds[arg0].open) {
            frame[14] = (uint64_t)-9;
            return 0;
        }
        if (!arg1 || !user_writable(arg1, arg2)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        struct knx_fd *kfd = &task->fds[arg0];
        uint64_t remaining = kfd->offset < kfd->size ? kfd->size - kfd->offset : 0;
        if (arg2 > remaining)
            arg2 = remaining;
        if (arg2 == 0) {
            frame[14] = 0;
            return 0;
        }
        int64_t got = knx_vfs_read(kfd->node, kfd->offset, (void *)arg1, arg2);
        if (got < 0) {
            frame[14] = (uint64_t)-5;
            return 0;
        }
        kfd->offset += (uint64_t)got;
        frame[14] = (uint64_t)got;
        return 0;
    }
    if (number == 16) {
        if (arg0 >= KNX_FD_MAX || !task->fds[arg0].open) {
            frame[14] = (uint64_t)-9;
            return 0;
        }
        task->fds[arg0].open = 0;
        task->fds[arg0].node = 0;
        task->fds[arg0].offset = 0;
        task->fds[arg0].size = 0;
        frame[14] = 0;
        return 0;
    }
    if (number == 18) {
        if (!arg0 || !arg1 || arg1 > 0x10000 || !user_writable(arg0, arg1)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        if (task->con_ready) {

            task->frame = frame;
            console_deliver_line(task);
            return 0;
        }
        lock_tasks();
        task->con_len = 0;
        task->con_ready = 0;
        task->frame = frame;
        console_waiter = task;

        console_owner = task;
        task->state = TASK_BLOCKED;
        unlock_tasks();
        return 1;
    }
    if (number == 19) {
        if (!fb_ready) {
            frame[14] = (uint64_t)-19;
            return 0;
        }
        if (!arg0 || !user_writable(arg0, sizeof(struct knx_fb_info))) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        int64_t user_addr = knx_vm_map_framebuffer();
        if (user_addr < 0) {
            frame[14] = (uint64_t)user_addr;
            return 0;
        }
        struct knx_fb_info mapped = fb_info;
        mapped.addr = (uint64_t)user_addr;
        struct knx_fb_info *out = (struct knx_fb_info *)(uintptr_t)arg0;
        *out = mapped;
        frame[14] = 0;
        return 0;
    }
    if (number == 20) {
        if (!fb_ready) {
            frame[14] = (uint64_t)-19;
            return 0;
        }
        frame[14] = 0;
        return 0;
    }
    if (number == 21) {

        int64_t shared = knx_vm_map_shared(task->address_space, arg0);
        frame[14] = (uint64_t)shared;
        return 0;
    }
    if (number == 22) {
        knx_console_graphics_owner();
        frame[14] = 0;
        return 0;
    }
    if (number == 23) {
        if (arg1 && !user_readable(arg0, arg1)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        frame[14] = (uint64_t)(int64_t)knx_pcm_write((const uint8_t *)(uintptr_t)arg0, arg1);
        return 0;
    }

    if (number == 24) {
        frame[14] = (uint64_t)(int64_t)knx_net_socket((int)arg0, (int)arg1, (int)arg2);
        return 0;
    }
    if (number == 25) {
        frame[14] = (uint64_t)(int64_t)knx_net_bind((int)arg0, (uint32_t)arg1, (uint16_t)arg2);
        return 0;
    }
    if (number == 26) {
        frame[14] = (uint64_t)(int64_t)knx_net_sendto((int)arg0, (const void *)(uintptr_t)arg1, arg2, arg3, (uint32_t)arg4, (uint16_t)arg5);
        return 0;
    }
    if (number == 27) {
        frame[14] = (uint64_t)(int64_t)knx_net_recvfrom((int)arg0, (void *)(uintptr_t)arg1, arg2, arg3, (uint32_t *)(uintptr_t)arg4, (uint16_t *)(uintptr_t)arg5);
        return 0;
    }
    if (number == 28) {
        frame[14] = (uint64_t)(int64_t)knx_net_close((int)arg0);
        return 0;
    }
    if (number == 29) {
        knx_net_ifconfig();
        frame[14] = 0;
        return 0;
    }
    if (number == 30) {
        knx_net_netstat();
        frame[14] = 0;
        return 0;
    }
    if (number == 31) {
        knx_net_info();
        frame[14] = 0;
        return 0;
    }
    if (number == 32) {
        knx_net_ping((uint32_t)arg0);
        frame[14] = 0;
        return 0;
    }
    if (number == 33) {
        if (!arg0 || !user_writable(arg0, sizeof(struct knx_key_event))) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        if (task->key_ready) {

            task->frame = frame;
            key_deliver_event(task);
            task->key_ready = 0;
            return 0;
        }

        uint8_t code = 0, pressed = 0;
        uint16_t mods = 0;
        if (knx_key_event_read(&code, &pressed, &mods)) {
            task->key_code = code;
            task->key_pressed = pressed;
            task->key_mods = mods;
            task->key_ready = 1;
            key_deliver_event(task);
            task->key_ready = 0;
            return 0;
        }
        lock_tasks();
        task->frame = frame;
        task->key_deadline = arg1 ? knx_ticks() + arg1 : 0;
        key_waiter = task;
        task->state = TASK_BLOCKED;
        unlock_tasks();
        return 1;
    }
    if (number == 35) {

        frame[14] = (uint64_t)(int64_t)task_spawn(arg0, arg1, 0);
        return 0;
    }
    if (number == 37) {
        frame[14] = knx_boot_flags();
        return 0;
    }
    if (number == 36) {
        int pid = (int)arg0;
        if (!arg1 || !arg2 || !user_writable(arg1, arg2)) {
            frame[14] = (uint64_t)-14;
            return 0;
        }

        unsigned tidx = MAX_TASKS;
        for (unsigned i = 1; i < MAX_TASKS; ++i) {
            if (tasks[i].pid == (uint64_t)pid) { tidx = i; break; }
        }
        if (tidx == MAX_TASKS) {
            frame[14] = (uint64_t)-3;
            return 0;
        }
        int cap = tasks[tidx].cap_slot;
        if (cap < 0) {
            frame[14] = (uint64_t)-22;
            return 0;
        }

        uint64_t deadline = knx_ticks() + 500;
        while (tasks[tidx].state != TASK_EXITED && tasks[tidx].state != TASK_ZOMBIE &&
               knx_ticks() < deadline) {
            (void)knx_sys_yield();
        }

        unsigned len = knx_capture_len(cap);
        if (len > arg2) len = (unsigned)arg2;
        char *buf = (char *)(uintptr_t)arg1;
        for (unsigned i = 0; i < len; ++i)
            buf[i] = knx_capture_buf(cap)[i];

        capture_free(cap);
        tasks[tidx].cap_slot = -1;

        if (tasks[tidx].state == TASK_EXITED || tasks[tidx].state == TASK_ZOMBIE) {
            void *space = tasks[tidx].address_space;
            tasks[tidx].state = TASK_UNUSED;
            --task_count;
            if (space) knx_vm_destroy_user(space);
        }
        frame[14] = (uint64_t)len;
        return 0;
    }
    if (number == 34) {
        if (!arg0 || !user_writable(arg0, sizeof(struct knx_mouse_event))) {
            frame[14] = (uint64_t)-14;
            return 0;
        }
        struct knx_mouse_event ev;
        if (knx_mouse_event_read(&ev)) {
            struct knx_mouse_event *out = (struct knx_mouse_event *)(uintptr_t)arg0;
            *out = ev;
            frame[14] = 1;
        } else {
            frame[14] = 0;
        }
        return 0;
    }
    if (number == 6) {
        if (index == 0) {
            frame[14] = (uint64_t)-1;
            return 0;
        }
        lock_tasks();
        if (console_waiter == task) {
            console_waiter = 0;
            task->con_ready = 0;
            task->con_len = 0;
        }
        if (key_waiter == task) {
            key_waiter = 0;
            task->key_ready = 0;
            task->key_deadline = 0;
        }
        if (console_owner == task)
            console_owner = 0;
        task->exit_status = (int32_t)arg0;
        task->frame = frame;
        task->state = TASK_EXITED;
        --task_count;
        if (!process_has_live_thread(task->pid)) {
            task->state = TASK_ZOMBIE;
            wake_waiters(task->pid);
        }
        unlock_tasks();
        return 1;
    }
    frame[14] = (uint64_t)-38;
    return 0;
}

uint64_t knx_task_count(void) {
    lock_tasks();
    uint64_t count = task_count;
    unlock_tasks();
    return count;
}
uint64_t knx_task_syscalls(void) {
    return syscall_calls;
}

uint64_t knx_task_syscall_audit(unsigned number) {
    return number < 40 ? __atomic_load_n(&syscall_audit[number], __ATOMIC_RELAXED) : 0;
}
uint64_t knx_process_count(void) {
    lock_tasks();
    uint64_t count = 1;
    for (unsigned i = 1; i < MAX_TASKS; ++i) {
        if (tasks[i].state != TASK_RUNNABLE && tasks[i].state != TASK_RUNNING)
            continue;
        int first = 1;
        for (unsigned j = 1; j < i; ++j)
            if ((tasks[j].state == TASK_RUNNABLE || tasks[j].state == TASK_RUNNING) &&
                tasks[j].pid == tasks[i].pid)
                first = 0;
        if (first)
            ++count;
    }
    unlock_tasks();
    return count;
}
