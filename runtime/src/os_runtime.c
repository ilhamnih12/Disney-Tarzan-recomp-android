#include "tarzan_recomp.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/*
 * Native libultra services for the ahead-of-time recompiled CPU.
 * Hardware the original OS poked (PI, VI, SI, AI, SP) is implemented here.
 * Game MIPS is not interpreted.
 */

#define RDRAM_SIZE 0x800000u
#define MAX_THREADS 48
#define MAX_QUEUES 96
#define MAX_TIMERS 32

extern void tz_gfx_task(uint8_t* rdram, uint32_t task);
extern void tz_gfx_reset(void);
extern void tz_gfx_set_log(FILE* fp);
extern int tz_gfx_has_frame(void);
extern void tz_gfx_dimensions(int* w, int* h);
extern void tz_gfx_copy(uint32_t* dst, int max_pixels);
extern void tz_gfx_dump_hist(FILE* fp);

uint32_t tz_gfx_triangles;
uint32_t tz_gfx_fill_rects;
uint32_t tz_gfx_unknown;
uint32_t tz_gfx_tasks;
uint32_t tz_gfx_vtx;
uint32_t tz_gfx_texrects;
uint32_t tz_gfx_unresolved;

static uint8_t* g_rdram;
static uint8_t* g_rom;
static size_t g_rom_size;
uint8_t tz_rcp[TZ_RCP_END - TZ_RCP_BASE];
uint8_t* tz_cart_view;
size_t tz_cart_size;
uint32_t tz_mmio_scratch;
uint32_t tz_mmio_unmapped;
static char g_save_path[512];
static uint8_t g_eep[2048];
static int g_eep_dirty;
static FILE* g_log;
static int g_trace_left = 8000;

static pthread_mutex_t g_int_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int g_int_depth;

static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_quit;
static volatile int g_active = 1;
volatile uint32_t tz_mark;
static volatile uint32_t g_vi_frames;
static volatile int g_ai_queued;
static uint16_t g_buttons;
static int8_t g_stick_x, g_stick_y;

static uint32_t g_present[640 * 480];
static int g_present_w = 320;
static int g_present_h = 240;
static volatile int g_frame_serial;

static uint32_t g_count_origin_ns;

typedef struct TzThread {
    int used;
    uint32_t addr;
    uint32_t entry;
    uint32_t arg;
    uint32_t sp;
    int id;
    int priority;
    int state;
    int started;
    pthread_t thread;
} TzThread;

static TzThread g_threads[MAX_THREADS];
static __thread TzThread* g_self;

typedef struct TzQueue {
    int used;
    uint32_t addr;
    uint32_t msg_buf;
    int capacity;
    int count;
    int head;
    uint32_t slots[64];
    pthread_mutex_t mu;
    pthread_cond_t cv;
} TzQueue;

static TzQueue g_queues[MAX_QUEUES];

typedef struct {
    int used;
    uint32_t addr;
    uint64_t deadline;
    uint64_t interval;
    uint32_t queue;
    uint32_t msg;
} TzTimer;

static TzTimer g_timers[MAX_TIMERS];

static uint32_t g_event_queue[16];
static uint32_t g_event_msg[16];
static uint32_t g_vi_queue, g_vi_msg, g_vi_retrace = 1;
static uint32_t g_next_fb, g_cur_fb;
static int g_vi_black = 1;
static pthread_t g_vi_thread;
static int g_vi_started;

static TzStats g_stats;

static void log_line(const char* msg) {
    if (!g_log) return;
    fputs(msg, g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

static void trace_os(const char* name) {
    g_stats.unimplemented += 0;
    if (g_trace_left <= 0 || !g_log) return;
    g_trace_left--;
    fprintf(g_log, "OS %s\n", name);
    fflush(g_log);
}

static uint32_t phys(gpr addr) { return (uint32_t)addr & 0x1FFFFFFFu; }

static uint32_t rd32(uint8_t* rdram, gpr addr) {
    uint32_t p = phys(addr);
    if (!rdram || p + 4 > RDRAM_SIZE) return 0;
    return *(uint32_t*)(rdram + p);
}

static void wr32(uint8_t* rdram, gpr addr, uint32_t value) {
    uint32_t p = phys(addr);
    if (!rdram || p + 4 > RDRAM_SIZE) return;
    *(uint32_t*)(rdram + p) = value;
}

static uint16_t rd16(uint8_t* rdram, gpr addr) {
    uint32_t p = phys(addr) ^ 2u;
    if (!rdram || p + 2 > RDRAM_SIZE) return 0;
    return *(uint16_t*)(rdram + p);
}

static void wr16(uint8_t* rdram, gpr addr, uint16_t value) {
    uint32_t p = phys(addr) ^ 2u;
    if (!rdram || p + 2 > RDRAM_SIZE) return;
    *(uint16_t*)(rdram + p) = value;
}

static void wr8(uint8_t* rdram, gpr addr, uint8_t value) {
    uint32_t p = phys(addr) ^ 3u;
    if (!rdram || p >= RDRAM_SIZE) return;
    rdram[p] = value;
}

static uint64_t n64_count(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (ns - g_count_origin_ns) * 46875ull / 1000000ull;
}

static TzQueue* queue_get(uint32_t addr, int create) {
    if (addr == 0) return NULL;
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].used && g_queues[i].addr == addr) return &g_queues[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (!g_queues[i].used) {
            memset(&g_queues[i], 0, sizeof(g_queues[i]));
            g_queues[i].used = 1;
            g_queues[i].addr = addr;
            g_queues[i].capacity = 1;
            pthread_mutex_init(&g_queues[i].mu, NULL);
            pthread_cond_init(&g_queues[i].cv, NULL);
            return &g_queues[i];
        }
    }
    return NULL;
}

static int queue_push(TzQueue* q, uint32_t msg, int block) {
    if (!q) return -1;
    pthread_mutex_lock(&q->mu);
    while (q->count >= q->capacity || q->count >= 64) {
        if (!block || g_quit) {
            pthread_mutex_unlock(&q->mu);
            return -1;
        }
        pthread_cond_wait(&q->cv, &q->mu);
    }
    int idx = (q->head + q->count) % 64;
    q->slots[idx] = msg;
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static int queue_pop(TzQueue* q, uint32_t* msg, int block) {
    if (!q) return -1;
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) {
        if (!block || g_quit) {
            static int early;
            if (early < 4 && block) {
                early++;
                fprintf(stderr, "POP early block=%d quit=%d addr=%p count=%d\n", block, g_quit, (void*)&g_quit, q->count);
            }
            pthread_mutex_unlock(&q->mu);
            return -1;
        }
        pthread_cond_wait(&q->cv, &q->mu);
    }
    *msg = q->slots[q->head];
    q->head = (q->head + 1) % 64;
    q->count--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static void send_event(int event) {
    if ((unsigned)event >= 16) return;
    if (g_event_queue[event]) {
        TzQueue* q = queue_get(g_event_queue[event], 0);
        queue_push(q, g_event_msg[event], 0);
    }
}

static void publish_frame(void) {
    int w = 320, h = 240;
    tz_gfx_dimensions(&w, &h);
    if (w < 1) w = 320;
    if (h < 1) h = 240;
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    pthread_mutex_lock(&g_state_lock);
    g_present_w = w;
    g_present_h = h;
    if (tz_gfx_has_frame() && !g_vi_black) {
        tz_gfx_copy(g_present, w * h);
    }
    g_frame_serial++;
    pthread_mutex_unlock(&g_state_lock);
}

static void fire_timers(uint64_t now) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].used) continue;
        if (now < g_timers[i].deadline) continue;
        TzQueue* q = queue_get(g_timers[i].queue, 0);
        queue_push(q, g_timers[i].msg, 0);
        if (g_timers[i].interval) g_timers[i].deadline += g_timers[i].interval;
        else g_timers[i].used = 0;
    }
}

static void* vi_thread_main(void* arg) {
    (void)arg;
    uint32_t ticks = 0;
    while (!g_quit) {
        struct timespec ts = {0, g_active ? 16666667 : 50000000};
        nanosleep(&ts, NULL);
        if (!g_active) continue;
        ticks++;
        if (ticks == 30 || ticks == 90 || ticks == 180) {
            fprintf(stderr, "MARK %u tick=%u dma=%u count=%08X cb=%08X\n", tz_mark, ticks,
                    g_stats.dma_bytes, g_rdram ? rd32(g_rdram, 0x80089234) : 0,
                    g_rdram ? rd32(g_rdram, 0x8005B940) : 0);
        }
        g_vi_frames++;
        g_stats.vi_frames = g_vi_frames;
        *(uint32_t*)(tz_rcp + (0x04400010u - TZ_RCP_BASE)) = (ticks * 2) & 0x3FF;
        fire_timers(n64_count());
        if (g_vi_retrace == 0 || (ticks % g_vi_retrace) == 0) {
            if (g_vi_queue) {
                TzQueue* q = queue_get(g_vi_queue, 0);
                int rc = queue_push(q, g_vi_msg, 0);
                if (ticks == g_vi_retrace || ticks == g_vi_retrace * 2) {
                    fprintf(stderr, "VI push q=%08X rc=%d count=%d\n", g_vi_queue, rc, q ? q->count : -1);
                }
            } else if (ticks == 30) {
                fprintf(stderr, "VI no queue yet\n");
            }
            send_event(7); /* OS_EVENT_VI */
            /* The game's retrace thread also increments this. If that thread blocks
             * inside its callback, the frame waiter still has to observe vblanks. */
            if (g_rdram) {
                uint32_t c = rd32(g_rdram, 0x80089234);
                wr32(g_rdram, 0x80089234, c + 1);
            }
            /* Game frame-sync waits on this queue. The retrace thread posts it,
             * but that thread can block inside its audio callback. */
            {
                TzQueue* retrace = queue_get(0x800A6AE0u, 0);
                if (retrace) queue_push(retrace, 0, 0);
            }
            if (g_ai_queued > 0) {
                g_ai_queued--;
                send_event(6); /* OS_EVENT_AI: one buffer finished */
            }
        }
        publish_frame();
    }
    return NULL;
}

static void ensure_vi(void) {
    if (g_vi_started) return;
    g_vi_started = 1;
    pthread_create(&g_vi_thread, NULL, vi_thread_main, NULL);
}

static void* thread_main(void* arg) {
    TzThread* t = arg;
    g_self = t;
    t->state = 4; /* RUNNING */
    recomp_context ctx;
    uint32_t f_odd[64];
    memset(&ctx, 0, sizeof(ctx));
    memset(f_odd, 0, sizeof(f_odd));
    ctx.f_odd = f_odd;
    ctx.r29 = (gpr)(int32_t)t->sp;
    ctx.r4 = (gpr)(int32_t)t->arg;
    ctx.r31 = 0;
    recomp_func_t* fn = get_function((int32_t)t->entry);
    if (g_log) {
        fprintf(g_log, "THREAD id=%d entry=%08X sp=%08X arg=%08X fn=%p\n",
                t->id, t->entry, t->sp, t->arg, (void*)fn);
        fflush(g_log);
    }
    if (fn) fn(g_rdram, &ctx);
    else if (g_log) {
        fprintf(g_log, "MISSING entry %08X\n", t->entry);
        fflush(g_log);
    }
    t->state = 1;
    return NULL;
}

static void dma_words(uint8_t* rdram, uint32_t vaddr, uint32_t rom_off, uint32_t nbytes, int to_rdram) {
    if (nbytes > 0x800000u) nbytes = 0x800000u;
    for (uint32_t i = 0; i < nbytes; i += 4) {
        uint32_t n = nbytes - i >= 4 ? 4 : nbytes - i;
        if (to_rdram) {
            uint32_t word = 0;
            if (rom_off + i + 4 <= g_rom_size) {
                word = ((uint32_t)g_rom[rom_off + i] << 24) | ((uint32_t)g_rom[rom_off + i + 1] << 16) |
                       ((uint32_t)g_rom[rom_off + i + 2] << 8) | (uint32_t)g_rom[rom_off + i + 3];
            }
            if (n == 4) wr32(rdram, vaddr + i, word);
            else {
                for (uint32_t b = 0; b < n; b++) wr8(rdram, vaddr + i + b, (uint8_t)(word >> (24 - 8 * b)));
            }
        }
    }
    g_stats.dma_bytes += nbytes;
}

static uint32_t cart_to_rom(uint32_t dev) {
    if (dev >= 0x10000000u && dev < 0x20000000u) return dev - 0x10000000u;
    if (dev >= 0xB0000000u) return dev - 0xB0000000u;
    return dev;
}

static void load_eep(void) {
    memset(g_eep, 0, sizeof(g_eep));
    FILE* fp = fopen(g_save_path, "rb");
    if (!fp) return;
    fread(g_eep, 1, sizeof(g_eep), fp);
    fclose(fp);
}

static void save_eep(void) {
    if (!g_eep_dirty || !g_save_path[0]) return;
    if (tz_atomic_write(g_save_path, g_eep, 512) == 0) g_eep_dirty = 0;
}

static void boot_info(uint8_t* rdram) {
    wr32(rdram, 0x80000300u, 1);          /* NTSC */
    wr32(rdram, 0x80000304u, 0);          /* cartridge */
    wr32(rdram, 0x80000308u, 0xB0000000u);
    wr32(rdram, 0x8000030Cu, 0);          /* cold reset */
    wr32(rdram, 0x80000310u, 0x6102u);
    wr32(rdram, 0x80000314u, 0x144Bu);
    wr32(rdram, 0x80000318u, RDRAM_SIZE); /* 8 MiB */
}

int tz_init(const uint8_t* rom, size_t rom_size, const char* save_dir) {
    if (!rom || rom_size < 0x1000) return -1;
    g_rdram = calloc(1, RDRAM_SIZE);
    g_rom = malloc(rom_size);
    if (!g_rdram || !g_rom) return -1;
    memcpy(g_rom, rom, rom_size);
    g_rom_size = rom_size;
    if (save_dir && save_dir[0]) {
        snprintf(g_save_path, sizeof(g_save_path), "%s/tarzan.eep", save_dir);
    }
    load_eep();
    free(tz_cart_view);
    tz_cart_view = malloc(rom_size ? rom_size : 1);
    tz_cart_size = rom_size;
    if (tz_cart_view) {
        for (size_t i = 0; i + 4 <= rom_size; i += 4) {
            uint32_t word = ((uint32_t)rom[i] << 24) | ((uint32_t)rom[i + 1] << 16) |
                            ((uint32_t)rom[i + 2] << 8) | rom[i + 3];
            memcpy(tz_cart_view + i, &word, 4);
        }
    }
    memset(tz_rcp, 0, sizeof(tz_rcp));
    /* SP halted, PI/SI/AI idle. VI current line is updated by the VI thread. */
    *(uint32_t*)(tz_rcp + (0x04040010u - TZ_RCP_BASE)) = 1;
    boot_info(g_rdram);
    /* IPL3 image ends where the entry's BSS clear begins. */
    uint32_t copy = 0x64ED0u;
    if (rom_size >= 0x1008) {
        uint32_t w0 = ((uint32_t)rom[0x1000] << 24) | ((uint32_t)rom[0x1001] << 16) |
                      ((uint32_t)rom[0x1002] << 8) | rom[0x1003];
        uint32_t w1 = ((uint32_t)rom[0x1004] << 24) | ((uint32_t)rom[0x1005] << 16) |
                      ((uint32_t)rom[0x1006] << 8) | rom[0x1007];
        if ((w0 >> 26) == 0x0F && (w1 >> 26) == 9) {
            int32_t lo = (int16_t)(w1 & 0xFFFF);
            uint32_t bss = ((w0 & 0xFFFF) << 16) + (uint32_t)lo;
            if (bss > 0x80000400u && bss < 0x80800000u) copy = bss - 0x80000400u;
        }
    }
    if (0x1000u + copy > rom_size) copy = (uint32_t)(rom_size - 0x1000u);
    dma_words(g_rdram, 0x80000400u, 0x1000u, copy, 1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_count_origin_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    tz_gfx_reset();
    ensure_vi();
    return 0;
}

void tz_set_log(FILE* fp) {
    g_log = fp;
    tz_gfx_set_log(fp);
}

void tz_request_quit(void) {
    g_quit = 1;
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].used) pthread_cond_broadcast(&g_queues[i].cv);
    }
}

int tz_should_quit(void) { return g_quit; }

void tz_set_active(int active) { g_active = active ? 1 : 0; }

int tz_frame_serial(void) { return g_frame_serial; }
int tz_frame_width(void) { return g_present_w; }
int tz_frame_height(void) { return g_present_h; }

void tz_copy_frame(uint32_t* dst, int max_pixels) {
    pthread_mutex_lock(&g_state_lock);
    int n = g_present_w * g_present_h;
    if (n > max_pixels) n = max_pixels;
    if (n > 0) memcpy(dst, g_present, (size_t)n * sizeof(uint32_t));
    pthread_mutex_unlock(&g_state_lock);
}

void tz_set_input(uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    pthread_mutex_lock(&g_state_lock);
    g_buttons = buttons;
    g_stick_x = stick_x;
    g_stick_y = stick_y;
    pthread_mutex_unlock(&g_state_lock);
}

void tz_get_stats(TzStats* out) {
    if (!out) return;
    *out = g_stats;
    out->triangles = tz_gfx_triangles;
    out->fill_rects = tz_gfx_fill_rects;
    out->unknown_cmds = tz_gfx_unknown;
    out->gfx_tasks = tz_gfx_tasks;
    out->vi_frames = g_vi_frames;
    out->vtx = tz_gfx_vtx;
    out->tex_rects = tz_gfx_texrects;
    out->unresolved_dl = tz_gfx_unresolved;
}

void tz_dump_gfx_hist(FILE* fp) { tz_gfx_dump_hist(fp); }

static void* entry_thread(void* arg) {
    (void)arg;
    recomp_context ctx;
    uint32_t f_odd[64];
    memset(&ctx, 0, sizeof(ctx));
    memset(f_odd, 0, sizeof(f_odd));
    ctx.f_odd = f_odd;
    ctx.r29 = (gpr)(int32_t)0x800907E0u;
    extern void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
    if (g_log) log_line("ENTRY recomp_entrypoint");
    recomp_entrypoint(g_rdram, &ctx);
    if (g_log) log_line("ENTRY returned");
    return NULL;
}

static pthread_t g_entry_thread;
static int g_entry_started;

void tz_start(void) {
    if (g_entry_started) return;
    g_entry_started = 1;
    pthread_create(&g_entry_thread, NULL, entry_thread, NULL);
}

void tz_shutdown(void) {
    tz_request_quit();
    save_eep();
}

static void missing_lookup(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    g_stats.unimplemented++;
    if (ctx) ctx->r2 = 0;
}

recomp_func_t* tz_lookup(int32_t vram) {
    recomp_func_t* fn = get_function(vram);
    if (fn) return fn;
    g_stats.unimplemented++;
    if (g_log && g_stats.unimplemented <= 48) {
        fprintf(g_log, "LOOKUP miss %08X\n", (uint32_t)vram);
        fflush(g_log);
    }
    return missing_lookup;
}

void recomp_unimplemented(uint8_t* rdram, recomp_context* ctx, uint32_t vram) {
    (void)rdram;
    (void)ctx;
    g_stats.unimplemented++;
    if (g_log && g_stats.unimplemented <= 40) {
        fprintf(g_log, "UNIMPLEMENTED %08X\n", vram);
        fflush(g_log);
    }
}

void pause_self(uint8_t* rdram) {
    (void)rdram;
    if (g_log) log_line("pause_self");
    while (!g_quit) {
        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
}

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    if (g_log) fprintf(g_log, "SWITCH %s %08X %08X\n", func ? func : "?", vram, jtbl);
}

void do_break(uint32_t vram) {
    if (g_log) fprintf(g_log, "BREAK %08X\n", vram);
}

void cop0_status_write(recomp_context* ctx, gpr value) {
    (void)ctx;
    (void)value;
}

gpr cop0_status_read(recomp_context* ctx) {
    (void)ctx;
    return 0x24000001ull; /* IE | CU1, BEV clear */
}

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    if (g_log) fprintf(g_log, "SYSCALL %08X\n", (uint32_t)instruction_vram);
}

/* ---- libultra replacements (strong; override weak_shims.c) ---- */

void __osInitialize_common_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("__osInitialize_common");
    boot_info(rdram);
    if (ctx) ctx->r2 = 0;
}

void osInitialize_recomp(uint8_t* rdram, recomp_context* ctx) {
    __osInitialize_common_recomp(rdram, ctx);
}

void osGetMemSize_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (ctx) ctx->r2 = RDRAM_SIZE;
    (void)rdram;
}

void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osCreateViManager");
    (void)rdram;
    (void)ctx;
    ensure_vi();
}

void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osViBlack");
    (void)rdram;
    g_vi_black = ctx && (ctx->r4 & 0xFF) ? 1 : 0;
    if (g_log) fprintf(g_log, "  viblack %u\n", g_vi_black);
}

void __osViSwapContext_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("__osViSwapContext");
    (void)rdram;
    (void)ctx;
    g_vi_black = 0;
    publish_frame();
}

void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}

void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    /* Game copy at 0x8001FC48 returns the SI/audio lock token. */
    (void)rdram;
    TzQueue* q = queue_get(0x800653C0u, 0);
    int rc = queue_push(q, 0, 0);
    if (ctx) ctx->r2 = (gpr)(int64_t)rc;
}

void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("__osSiRawStartDma");
    if (ctx && ((uint32_t)ctx->r4 & 1u) == 0 && ctx->r5) {
        /* Read from PIF: present one standard controller on channel 0. */
        uint32_t buf = (uint32_t)ctx->r5;
        for (uint32_t i = 0; i < 64; i++) wr8(rdram, buf + i, 0);
        wr8(rdram, buf + 0, 0x00);
        wr8(rdram, buf + 1, 0x05); /* CONT_TYPE_NORMAL */
        wr8(rdram, buf + 2, 0x00);
        wr8(rdram, buf + 3, 0x00);
    }
    send_event(5);
    if (ctx) ctx->r2 = 0;
}

void __osPackRequestData_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

void __osContGetInitData_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("__osContGetInitData");
    if (!ctx) return;
    if (ctx->r4) wr8(rdram, ctx->r4, 1);
    if (ctx->r5) {
        wr16(rdram, ctx->r5 + 0, 5);
        wr8(rdram, ctx->r5 + 2, 0);
        wr8(rdram, ctx->r5 + 3, 0);
    }
    if (ctx) ctx->r2 = 0;
}

void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osViSetMode");
    (void)rdram;
    (void)ctx;
}

void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osViSetEvent");
    (void)rdram;
    if (!ctx) return;
    g_vi_queue = (uint32_t)ctx->r4;
    g_vi_msg = (uint32_t)ctx->r5;
    g_vi_retrace = (uint32_t)ctx->r6 ? (uint32_t)ctx->r6 : 1;
    queue_get(g_vi_queue, 1);
    fprintf(stderr, "ViSetEvent q=%08X retrace=%u\n", g_vi_queue, g_vi_retrace);
    ensure_vi();
}

void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osViSwapBuffer");
    (void)rdram;
    if (!ctx) return;
    g_cur_fb = g_next_fb;
    g_next_fb = (uint32_t)ctx->r4;
    g_vi_black = 0;
    publish_frame();
    fprintf(stderr, "swap fb=%08X count=%08X saved=%08X\n", (uint32_t)ctx->r4, rd32(rdram, 0x80089234), rd32(rdram, 0x8005B490));
}

void osViGetNextFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = g_next_fb;
}

void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = g_cur_fb;
}

void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

void osViSetXScale_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osViSetYScale_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osViRepeatLine_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osCreateMesgQueue");
    if (!ctx) return;
    uint32_t addr = (uint32_t)ctx->r4;
    TzQueue* q = queue_get(addr, 1);
    if (!q) return;
    q->msg_buf = (uint32_t)ctx->r5;
    q->capacity = (int)ctx->r6;
    if (q->capacity < 1) q->capacity = 1;
    if (q->capacity > 64) q->capacity = 64;
    q->count = 0;
    q->head = 0;
    wr32(rdram, addr + 0x00, 0);
    wr32(rdram, addr + 0x04, 0);
    wr32(rdram, addr + 0x08, 0);
    wr32(rdram, addr + 0x0C, 0);
    wr32(rdram, addr + 0x10, (uint32_t)q->capacity);
    wr32(rdram, addr + 0x14, q->msg_buf);
}

void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    uint32_t msg = 0;
    int block = ((uint32_t)ctx->r6) != 0;
    uint32_t qaddr = (uint32_t)ctx->r4;
    TzQueue* q = queue_get(qaddr, 0);
    static int recv_logs;
    int log_this = recv_logs < 8;
    if (log_this) {
        recv_logs++;
        fprintf(stderr, "RECV q=%08X block=%d found=%d count=%d\n", qaddr, block, q != NULL, q ? q->count : -1);
    }
    int rc = queue_pop(q, &msg, block);
    if (log_this) fprintf(stderr, "RECV done q=%08X rc=%d\n", qaddr, rc);
    if (qaddr == 0x8008335Cu) {
        static int lk;
        if (lk < 12) {
            lk++;
            fprintf(stderr, "LOCK recv block=%d rc=%d tid=%d\n", block, rc, g_self ? g_self->id : -1);
        }
    }
    if (rc == 0 && ctx->r5) wr32(rdram, ctx->r5, msg);
    ctx->r2 = (gpr)(int64_t)rc;
}

void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    int block = ((uint32_t)ctx->r6) != 0;
    uint32_t qaddr = (uint32_t)ctx->r4;
    TzQueue* q = queue_get(qaddr, 0);
    int rc = queue_push(q, (uint32_t)ctx->r5, block);
    if (qaddr == 0x8008335Cu) {
        static int lk;
        if (lk < 12) {
            lk++;
            fprintf(stderr, "LOCK send block=%d rc=%d tid=%d\n", block, rc, g_self ? g_self->id : -1);
        }
    }
    static int send_logs;
    if (qaddr == 0x800A6AE0 && send_logs < 8) {
        send_logs++;
        fprintf(stderr, "SEND q=%08X block=%d rc=%d\n", qaddr, block, rc);
    }
    ctx->r2 = (gpr)(int64_t)rc;
}

void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    osSendMesg_recomp(rdram, ctx);
}

void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osSetEventMesg");
    (void)rdram;
    if (!ctx) return;
    uint32_t event = (uint32_t)ctx->r4;
    if (event < 16) {
        g_event_queue[event] = (uint32_t)ctx->r5;
        g_event_msg[event] = (uint32_t)ctx->r6;
        if (g_event_queue[event]) queue_get(g_event_queue[event], 1);
    }
}

void osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osCreateThread");
    if (!ctx) return;
    uint32_t addr = (uint32_t)ctx->r4;
    uint32_t sp = rd32(rdram, ctx->r29 + 0x10);
    uint32_t pri = rd32(rdram, ctx->r29 + 0x14);
    TzThread* slot = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].used || g_threads[i].addr == addr) {
            slot = &g_threads[i];
            break;
        }
    }
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->addr = addr;
    slot->id = (int)ctx->r5;
    slot->entry = (uint32_t)ctx->r6;
    slot->arg = (uint32_t)ctx->r7;
    slot->sp = sp;
    slot->priority = (int)pri;
    slot->state = 1;
    wr32(rdram, addr + 0x04, pri);
    wr32(rdram, addr + 0x14, (uint32_t)slot->id);
    wr16(rdram, addr + 0x10, 1);
    if (g_log) {
        fprintf(g_log, "  create id=%d entry=%08X sp=%08X pri=%u\n", slot->id, slot->entry, slot->sp, pri);
        fflush(g_log);
    }
}

void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osStartThread");
    (void)rdram;
    if (!ctx) return;
    uint32_t addr = (uint32_t)ctx->r4;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].used || g_threads[i].addr != addr) continue;
        if (g_threads[i].started) return;
        g_threads[i].started = 1;
        g_threads[i].state = 2;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
        pthread_create(&g_threads[i].thread, &attr, thread_main, &g_threads[i]);
        pthread_attr_destroy(&attr);
        g_stats.threads_started++;
        return;
    }
}

void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osDestroyThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx && (uint32_t)ctx->r4 == 0 && g_self) g_self->state = 1;
}

void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osSetThreadPri");
    (void)rdram;
    if (!ctx) return;
    TzThread* t = g_self;
    if ((uint32_t)ctx->r4 != 0) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (g_threads[i].used && g_threads[i].addr == (uint32_t)ctx->r4) t = &g_threads[i];
        }
    }
    if (!t) return;
    t->priority = (int)ctx->r5;
    /* The idle thread drops itself to priority 0 and then spins. Park it. */
    if (t == g_self && t->priority <= 0) {
        while (!g_quit && g_self && g_self->priority <= 0) {
            struct timespec ts = {0, 20000000};
            nanosleep(&ts, NULL);
        }
    }
}

void osGetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    int pri = g_self ? g_self->priority : 0;
    ctx->r2 = (gpr)(int64_t)pri;
}

void osGetThreadId_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = g_self ? (gpr)(int64_t)g_self->id : 0;
}

void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    sched_yield();
}

void __osDisableInt_recomp(uint8_t* rdram, recomp_context* ctx) {
    /* No-op. A mutex here deadlocks if the holder blocks in osRecvMesg.
     * VI/SI callbacks run on their own threads and take the queue lock. */
    (void)rdram;
    (void)g_int_lock;
    if (ctx) ctx->r2 = 1;
}

void __osRestoreInt_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

void osSetIntMask_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0x0000FF01ull;
}

void osGetCount_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = (uint32_t)n64_count();
}

void osSetCount_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

void osGetTime_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    uint64_t t = n64_count();
    ctx->r2 = (gpr)(t >> 32);
    ctx->r3 = (gpr)(uint32_t)t;
}

void osSetTime_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

void osSetTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osSetTimer");
    if (!ctx) return;
    uint64_t countdown = ((uint64_t)(uint32_t)ctx->r5 << 32) | (uint32_t)ctx->r6;
    uint64_t interval = ((uint64_t)(uint32_t)ctx->r7 << 32) | (uint32_t)rd32(rdram, ctx->r29 + 0x10);
    uint32_t mq = rd32(rdram, ctx->r29 + 0x14);
    uint32_t msg = rd32(rdram, ctx->r29 + 0x18);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].used && g_timers[i].addr != (uint32_t)ctx->r4) continue;
        g_timers[i].used = 1;
        g_timers[i].addr = (uint32_t)ctx->r4;
        g_timers[i].deadline = n64_count() + (countdown ? countdown : 1);
        g_timers[i].interval = interval;
        g_timers[i].queue = mq;
        g_timers[i].msg = msg;
        break;
    }
    if (ctx) ctx->r2 = 0;
}

void osStopTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].used && g_timers[i].addr == (uint32_t)ctx->r4) g_timers[i].used = 0;
    }
    ctx->r2 = 0;
}

void osVirtualToPhysical_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    uint32_t a = (uint32_t)ctx->r4;
    ctx->r2 = (a >= 0x80000000u && a < 0xC0000000u) ? (a & 0x1FFFFFFFu) : 0;
}

void osInvalDCache_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osWritebackDCache_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osWritebackDCacheAll_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osCreatePiManager");
    (void)rdram;
    (void)ctx;
}

void osPiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}

void osPiGetCmdQueue_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}

static void pi_dma(uint8_t* rdram, uint32_t dev, uint32_t vaddr, uint32_t nbytes, int direction, uint32_t mq) {
    uint32_t rom_off = cart_to_rom(dev);
    if (direction == 0) dma_words(rdram, vaddr, rom_off, nbytes, 1);
    if (g_log && g_stats.dma_bytes < 0x200000) {
        fprintf(g_log, "DMA dev=%08X vaddr=%08X n=%u dir=%d\n", dev, vaddr, nbytes, direction);
        fflush(g_log);
    }
    if (mq) {
        TzQueue* q = queue_get(mq, 0);
        queue_push(q, 0, 0);
    }
    send_event(8); /* OS_EVENT_PI */
}

void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osPiStartDma");
    if (!ctx) return;
    uint32_t dev = (uint32_t)ctx->r7;
    uint32_t vaddr = rd32(rdram, ctx->r29 + 0x10);
    uint32_t nbytes = rd32(rdram, ctx->r29 + 0x14);
    uint32_t mq = rd32(rdram, ctx->r29 + 0x18);
    pi_dma(rdram, dev, vaddr, nbytes, (int)ctx->r6, mq);
    ctx->r2 = 0;
}

void osEPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osEPiStartDma");
    if (!ctx) return;
    uint32_t mb = (uint32_t)ctx->r5;
    uint32_t dram = rd32(rdram, mb + 0x08);
    uint32_t dev = rd32(rdram, mb + 0x0C);
    uint32_t size = rd32(rdram, mb + 0x10);
    uint32_t mq = rd32(rdram, mb + 0x04); /* OSIoMesgHdr.retQueue */
    pi_dma(rdram, dev, dram, size, (int)ctx->r6, 0);
    if (mq) {
        TzQueue* q = queue_get(mq, 0);
        queue_push(q, mb, 0);
    }
    ctx->r2 = 0;
}

void osEPiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    osEPiStartDma_recomp(rdram, ctx);
}

void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0x80000000u;
}

void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    uint32_t dev = cart_to_rom((uint32_t)ctx->r4);
    uint32_t word = 0;
    if (dev + 4 <= g_rom_size) {
        word = ((uint32_t)g_rom[dev] << 24) | ((uint32_t)g_rom[dev + 1] << 16) |
               ((uint32_t)g_rom[dev + 2] << 8) | g_rom[dev + 3];
    }
    if (ctx->r5) wr32(rdram, ctx->r5, word);
    ctx->r2 = 0;
}

void osEPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    /* a0 handle, a1 devAddr, a2 data ptr — shift to osPiReadIo layout. */
    if (!ctx) return;
    gpr saved = ctx->r4;
    ctx->r4 = ctx->r5;
    ctx->r5 = ctx->r6;
    osPiReadIo_recomp(rdram, ctx);
    ctx->r4 = saved;
}

void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osSpTaskLoad");
    (void)rdram;
    (void)ctx;
}

void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osSpTaskStartGo");
    if (!ctx) return;
    uint32_t task = (uint32_t)ctx->r4;
    uint32_t type = rd32(rdram, task);
    static int sp_logs;
    if (type == 1 || sp_logs < 4) {
        if (sp_logs < 12) {
            sp_logs++;
            fprintf(stderr, "SP %08X type=%08X +30=%08X\n", task, type, rd32(rdram, task + 0x30));
        }
    }
    if (type == 1) {
        tz_gfx_task(rdram, task);
        publish_frame();
    } else if (type == 2) {
        g_stats.audio_tasks++;
    }
    send_event(4); /* SP */
    send_event(9); /* DP */
}

void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}
void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void __osSpGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}
void __osSpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osDpGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 0;
}
void osDpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }
void osDpSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 32000;
}

void osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    /* AI_STATUS_FIFO_FULL when both DMA slots are busy. */
    if (ctx) ctx->r2 = g_ai_queued >= 2 ? 0x80000000u : 0;
}

void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = g_ai_queued ? 0x100 : 0;
}

void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!ctx) return;
    if (g_ai_queued >= 2) {
        ctx->r2 = (gpr)(int64_t)-1;
        return;
    }
    g_ai_queued++;
    g_stats.audio_tasks++;
    ctx->r2 = 0;
    /* Completion is posted from the VI thread, not here. Signaling immediately
     * makes the audio thread recv the event it just queued and spin. */
}

static void write_pad(uint8_t* rdram, uint32_t pad) {
    uint16_t buttons;
    int8_t sx, sy;
    pthread_mutex_lock(&g_state_lock);
    buttons = g_buttons;
    sx = g_stick_x;
    sy = g_stick_y;
    pthread_mutex_unlock(&g_state_lock);
    wr16(rdram, pad + 0, buttons);
    wr8(rdram, pad + 2, (uint8_t)sx);
    wr8(rdram, pad + 3, (uint8_t)sy);
    wr8(rdram, pad + 4, 0);
}

void osContInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    trace_os("osContInit");
    if (!ctx) return;
    if (ctx->r5) wr8(rdram, ctx->r5, 1);
    if (ctx->r6) {
        wr16(rdram, ctx->r6 + 0, 5); /* CONT_TYPE_NORMAL */
        wr8(rdram, ctx->r6 + 2, 0);
        wr8(rdram, ctx->r6 + 3, 0);
    }
    send_event(5);
    ctx->r2 = 0;
}

void osContStartQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    send_event(5);
}

void osContGetQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    if (ctx->r4) wr16(rdram, ctx->r4, 5);
}

void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    send_event(5);
}

void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    write_pad(rdram, (uint32_t)ctx->r4);
}

void osContSetCh_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 1;
}

static void eep_rw(uint8_t* rdram, uint32_t block, uint32_t buf, uint32_t nbytes, int write) {
    if (block * 8 + nbytes > sizeof(g_eep)) return;
    for (uint32_t i = 0; i < nbytes; i++) {
        if (write) {
            uint8_t b = 0;
            uint32_t p = phys(buf + i) ^ 3u;
            if (p < RDRAM_SIZE) b = rdram[p];
            g_eep[block * 8 + i] = b;
            g_eep_dirty = 1;
        } else {
            wr8(rdram, buf + i, g_eep[block * 8 + i]);
        }
    }
    if (write) save_eep();
}

void osEepromProbe_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) ctx->r2 = 1;
}

void osEepromRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    eep_rw(rdram, (uint32_t)ctx->r5, (uint32_t)ctx->r6, 8, 0);
    ctx->r2 = 0;
}

void osEepromWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    eep_rw(rdram, (uint32_t)ctx->r5, (uint32_t)ctx->r6, 8, 1);
    ctx->r2 = 0;
}

void osEepromLongRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    eep_rw(rdram, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7, 0);
    ctx->r2 = 0;
}

void osEepromLongWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx) return;
    eep_rw(rdram, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7, 1);
    ctx->r2 = 0;
}

void osSyncPrintf_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}
