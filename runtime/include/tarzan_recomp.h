#ifndef TARZAN_RECOMP_H
#define TARZAN_RECOMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * NDK r15 clang does not reliably emit upstream's `extern inline` bodies.
 * Predefine a plain noinline attribute before including the MIT recomp.h.
 */
#if defined(__clang__)
#define RECOMP_FUNC __attribute__((noinline, used))
#endif

#include "recomp.h"

/* Game data can be NaN, and MIPS I odd-register stores are expected. */
#undef NAN_CHECK
#define NAN_CHECK(val) ((void)(val))
#undef CHECK_FR
#define CHECK_FR(ctx, idx) ((void)(ctx), (void)(idx))

#ifdef __cplusplus
extern "C" {
#endif

void recomp_unimplemented(uint8_t* rdram, recomp_context* ctx, uint32_t vram);
recomp_func_t* tz_lookup(int32_t vram);

/* A missing indirect target must not crash the host. */
#undef LOOKUP_FUNC
#define LOOKUP_FUNC(val) tz_lookup((int32_t)(val))

/*
 * Stock MEM_* subtracts the KSEG0 base only. Tarzan also stores to KSEG1
 * RCP registers (AI/VI/PI/SI). Map those onto a shadow, and cart reads onto
 * a word-swapped copy of the ROM. The two macro arguments are added, so
 * either (offset, base) or (base, offset) works.
 */
#define TZ_RDRAM_SIZE 0x800000u
#define TZ_RCP_BASE   0x04000000u
#define TZ_RCP_END    0x04900000u

extern uint8_t tz_rcp[TZ_RCP_END - TZ_RCP_BASE];
extern uint8_t* tz_cart_view;
extern size_t tz_cart_size;
extern uint32_t tz_mmio_scratch;
extern uint32_t tz_mmio_unmapped;

static inline uint8_t* tz_host_ptr(uint8_t* rdram, gpr a, gpr b, uint32_t xor_mask) {
    uint32_t phys = (((uint32_t)(a + b)) & 0x1FFFFFFFu) ^ xor_mask;
    if (phys < TZ_RDRAM_SIZE && rdram) return rdram + phys;
    if (phys >= TZ_RCP_BASE && phys < TZ_RCP_END) return tz_rcp + (phys - TZ_RCP_BASE);
    if (tz_cart_view && phys >= 0x10000000u) {
        uint32_t off = phys - 0x10000000u;
        if (off < tz_cart_size) return tz_cart_view + off;
    }
    tz_mmio_unmapped++;
    return (uint8_t*)&tz_mmio_scratch;
}

#undef MEM_W
#undef MEM_H
#undef MEM_B
#undef MEM_HU
#undef MEM_BU
#undef MEM_WU
#undef SD
#undef LD

#define MEM_W(a, b)  (*(int32_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 0))
#define MEM_WU(a, b) (*(uint32_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 0))
#define MEM_H(a, b)  (*(int16_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 2))
#define MEM_HU(a, b) (*(uint16_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 2))
#define MEM_B(a, b)  (*(int8_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 3))
#define MEM_BU(a, b) (*(uint8_t*)tz_host_ptr(rdram, (gpr)(a), (gpr)(b), 3))

#define SD(val, offset, reg) do { \
    uint64_t _tz_sd = (uint64_t)(gpr)(val); \
    MEM_WU((reg), (gpr)(offset) + 4) = (uint32_t)_tz_sd; \
    MEM_WU((reg), (gpr)(offset) + 0) = (uint32_t)(_tz_sd >> 32); \
} while (0)

static inline uint64_t tz_load_double(uint8_t* rdram, gpr offset, gpr reg) {
    uint64_t lo = (uint32_t)MEM_WU(reg, offset + 4);
    uint64_t hi = (uint32_t)MEM_WU(reg, offset + 0);
    return lo | (hi << 32);
}

#define LD(offset, reg) tz_load_double(rdram, (gpr)(offset), (gpr)(reg))

extern volatile uint32_t tz_mark;

int tz_init(const uint8_t* rom, size_t rom_size, const char* save_dir);
void tz_start(void);
void tz_shutdown(void);
void tz_request_quit(void);
int tz_should_quit(void);

int tz_frame_serial(void);
int tz_frame_width(void);
int tz_frame_height(void);
void tz_copy_frame(uint32_t* dst, int max_pixels);
void tz_set_input(uint16_t buttons, int8_t stick_x, int8_t stick_y);
void tz_set_active(int active);
void tz_set_log(FILE* fp);

/* 0 = not the NTSC-U image. Does not copy or embed the ROM. */
void tz_sha256(const uint8_t* data, size_t len, uint8_t out[32]);
int tz_rom_is_tarzan_us(const uint8_t* data, size_t len);

/* Private-file write: path.tmp, flush, fsync, rename. 0 = success. */
int tz_atomic_write(const char* path, const void* data, size_t n);

enum { TZ_ZONE_BTN = 0, TZ_ZONE_STICK = 1, TZ_ZONE_DPAD = 2 };

typedef struct TzZone {
    int x, y, w, h;
    uint16_t buttons;
    int kind;
} TzZone;

void tz_touch_layout(int w, int h, int top_reserve, TzZone* out, int cap, int* count);
void tz_touch_eval(const TzZone* zones, int count, const int* xs, const int* ys, int npoints,
                   uint16_t* buttons, int8_t* stick_x, int8_t* stick_y, int* stick_active);

typedef struct TzStats {
    uint32_t vi_frames;
    uint32_t gfx_tasks;
    uint32_t audio_tasks;
    uint32_t triangles;
    uint32_t fill_rects;
    uint32_t dma_bytes;
    uint32_t unknown_cmds;
    uint32_t threads_started;
    uint32_t unimplemented;
    uint32_t vtx;
    uint32_t tex_rects;
    uint32_t unresolved_dl;
} TzStats;

void tz_get_stats(TzStats* out);

/* N64 button bits. */
#define TZ_BTN_A      0x8000u
#define TZ_BTN_B      0x4000u
#define TZ_BTN_Z      0x2000u
#define TZ_BTN_START  0x1000u
#define TZ_BTN_DUP    0x0800u
#define TZ_BTN_DDOWN  0x0400u
#define TZ_BTN_DLEFT  0x0200u
#define TZ_BTN_DRIGHT 0x0100u
#define TZ_BTN_L      0x0020u
#define TZ_BTN_R      0x0010u
#define TZ_BTN_CUP    0x0008u
#define TZ_BTN_CDOWN  0x0004u
#define TZ_BTN_CLEFT  0x0002u
#define TZ_BTN_CRIGHT 0x0001u

#ifdef __cplusplus
}
#endif

#endif
