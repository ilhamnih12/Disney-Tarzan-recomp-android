#include "tarzan_recomp.h"

#include <stdio.h>
#include <string.h>

/*
 * F3DEX2 display-list HLE.
 * The measured Tarzan task uses F3DEX2 opcodes (G_SETOTHERMODE_H = 0xE3,
 * G_MOVEWORD = 0xDB, G_DL = 0xDE). This is not RT64 and not Vulkan.
 * Triangles are drawn only when the list actually contains G_VTX/G_TRI*.
 */

#define GFX_W 640
#define GFX_H 480
#define GFX_STACK 12

extern uint32_t tz_gfx_triangles;
extern uint32_t tz_gfx_fill_rects;
extern uint32_t tz_gfx_unknown;
extern uint32_t tz_gfx_tasks;
extern uint32_t tz_gfx_vtx;
extern uint32_t tz_gfx_texrects;
extern uint32_t tz_gfx_unresolved;

static uint32_t pixels[GFX_W * GFX_H];
static int fb_w = 320;
static int fb_h = 240;
static int drew;

static float mv_stack[GFX_STACK][4][4];
static float pj_stack[4][4][4];
static int mv_sp;
static int pj_sp;
static int mtx_proj;

static float vscale[4] = {640.f, 480.f, 511.f, 0.f};
static float vtrans[4] = {640.f, 480.f, 511.f, 0.f};

typedef struct {
    float x, y, z, w;
    float s, t;
    uint8_t r, g, b, a;
    int clip;
} Vert;

static Vert verts[64];
static uint32_t fill_color = 0xFF000000u;
static uint32_t prim_color = 0xFFFFFFFFu;
static int geom_mode;
static uint32_t cmd_hist[256];
static int log_left = 24;
static FILE* gfx_log;
static uint32_t seg_base[16];
static int task_logs;
static int op_dump;

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static uint32_t phys(uint32_t addr) { return addr & 0x1FFFFFFFu; }

static uint32_t r32(uint8_t* rdram, uint32_t addr) {
    uint32_t p = phys(addr);
    if (p + 4 > 0x800000u) return 0;
    return *(uint32_t*)(rdram + p);
}

static int16_t r16(uint8_t* rdram, uint32_t addr) {
    uint32_t p = phys(addr) ^ 2u;
    if (p + 2 > 0x800000u) return 0;
    return *(int16_t*)(rdram + p);
}

static uint8_t r8(uint8_t* rdram, uint32_t addr) {
    uint32_t p = phys(addr) ^ 3u;
    if (p >= 0x800000u) return 0;
    return rdram[p];
}

static void ident(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.f;
}

static void mul_mat(const float a[4][4], const float b[4][4], float o[4][4]) {
    float t[4][4];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
        }
    }
    memcpy(o, t, sizeof(t));
}

static void load_mtx(uint8_t* rdram, uint32_t addr, float m[4][4]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int16_t ip = r16(rdram, addr + (uint32_t)((r * 4 + c) * 2));
            uint16_t fp = (uint16_t)r16(rdram, addr + 32u + (uint32_t)((r * 4 + c) * 2));
            m[r][c] = (float)ip + (float)fp / 65536.f;
        }
    }
}

static void put_px(int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h) return;
    if (fb_w > GFX_W || fb_h > GFX_H) return;
    pixels[y * fb_w + x] = color;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fb_w) x1 = fb_w - 1;
    if (y1 >= fb_h) y1 = fb_h - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) put_px(x, y, color);
    }
    tz_gfx_fill_rects++;
    drew = 1;
}

static void raster_tri(const Vert* a, const Vert* b, const Vert* c) {
    float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y, x2 = c->x, y2 = c->y;
    int minx = (int)(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int maxx = (int)(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
    int miny = (int)(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int maxy = (int)(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
    if (maxx < 0 || maxy < 0 || minx >= fb_w || miny >= fb_h) return;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= fb_w) maxx = fb_w - 1;
    if (maxy >= fb_h) maxy = fb_h - 1;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area > -0.5f && area < 0.5f) return;
    if (area < 0.f) {
        const Vert* t = b; b = c; c = t;
        x1 = b->x; y1 = b->y; x2 = c->x; y2 = c->y;
        area = -area;
    }
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
            float w1 = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
            float w2 = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
            if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) {
                float inv = 1.f / (w0 + w1 + w2);
                int r = (int)((w0 * a->r + w1 * b->r + w2 * c->r) * inv);
                int g = (int)((w0 * a->g + w1 * b->g + w2 * c->g) * inv);
                int bl = (int)((w0 * a->b + w1 * b->b + w2 * c->b) * inv);
                put_px(x, y, pack((uint8_t)r, (uint8_t)g, (uint8_t)bl, 255));
            }
        }
    }
    tz_gfx_triangles++;
    drew = 1;
}

static void xform(uint8_t* rdram, int slot, uint32_t addr) {
    (void)rdram;
    float x = (float)r16(rdram, addr + 0);
    float y = (float)r16(rdram, addr + 2);
    float z = (float)r16(rdram, addr + 4);
    float s = (float)r16(rdram, addr + 8);
    float t = (float)r16(rdram, addr + 10);
    uint8_t r = r8(rdram, addr + 12);
    uint8_t g = r8(rdram, addr + 13);
    uint8_t b = r8(rdram, addr + 14);
    uint8_t a = r8(rdram, addr + 15);
    float mv[4];
    float* m = &mv_stack[mv_sp][0][0];
    mv[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    mv[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    mv[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    mv[3] = m[12] * x + m[13] * y + m[14] * z + m[15];
    float* p = &pj_stack[pj_sp][0][0];
    float cx = p[0] * mv[0] + p[1] * mv[1] + p[2] * mv[2] + p[3] * mv[3];
    float cy = p[4] * mv[0] + p[5] * mv[1] + p[6] * mv[2] + p[7] * mv[3];
    float cz = p[8] * mv[0] + p[9] * mv[1] + p[10] * mv[2] + p[11] * mv[3];
    float cw = p[12] * mv[0] + p[13] * mv[1] + p[14] * mv[2] + p[15] * mv[3];
    Vert* v = &verts[slot & 63];
    v->w = cw;
    v->s = s;
    v->t = t;
    v->r = r; v->g = g; v->b = b; v->a = a;
    v->clip = cw < 0.1f;
    if (!v->clip) {
        float inv = 1.f / cw;
        float ndc_x = cx * inv;
        float ndc_y = cy * inv;
        v->x = (vscale[0] * ndc_x + vtrans[0]) * 0.25f;
        v->y = (vscale[1] * ndc_y + vtrans[1]) * 0.25f;
        /* N64 framebuffer Y grows downward; clip Y grows upward. */
        v->y = (float)(fb_h - 1) - v->y;
        v->z = cz * inv;
    }
}

static int idx_of(uint32_t packed, int shift) {
    return (int)(((packed >> shift) & 0xFFu) / 2u);
}

static int resolve_addr(uint32_t addr, uint32_t* out) {
    uint32_t top = addr >> 24;
    if (top == 0x80 || top == 0xA0) {
        *out = addr;
        return 1;
    }
    if (top >= 1 && top <= 15) {
        if (!seg_base[top]) return 0;
        *out = seg_base[top] + (addr & 0x00FFFFFFu);
        return 1;
    }
    *out = addr;
    return 1;
}

static void load_viewport(uint8_t* rdram, uint32_t addr) {
    uint32_t phys_addr = 0;
    if (!resolve_addr(addr, &phys_addr)) return;
    vscale[0] = (float)r16(rdram, phys_addr + 0);
    vscale[1] = (float)r16(rdram, phys_addr + 2);
    vtrans[0] = (float)r16(rdram, phys_addr + 8);
    vtrans[1] = (float)r16(rdram, phys_addr + 10);
}

static void draw_indexed(uint32_t packed) {
    int i0 = idx_of(packed, 16);
    int i1 = idx_of(packed, 8);
    int i2 = idx_of(packed, 0);
    if ((unsigned)i0 >= 64 || (unsigned)i1 >= 64 || (unsigned)i2 >= 64) return;
    if (verts[i0].clip || verts[i1].clip || verts[i2].clip) return;
    raster_tri(&verts[i0], &verts[i1], &verts[i2]);
}

static void run_dl(uint8_t* rdram, uint32_t addr, int depth, int budget);

static void one_cmd(uint8_t* rdram, uint32_t addr, int depth, int* branch_to, int* branched) {
    uint32_t w0 = r32(rdram, addr);
    uint32_t w1 = r32(rdram, addr + 4);
    uint8_t op = (uint8_t)(w0 >> 24);
    cmd_hist[op]++;
    if (op_dump < 20 && task_logs == 0) {
        fprintf(stderr, "OP %02X %08X %08X\n", op, w0, w1);
        op_dump++;
    }
    if (log_left > 0 && gfx_log) {
        fprintf(gfx_log, "DL %08X %02X %08X %08X\n", addr, op, w0, w1);
        log_left--;
    }
    switch (op) {
    case 0x00:
    case 0x02: /* MODIFYVTX */
    case 0x03: /* CULLDL */
    case 0x04: /* BRANCH_Z */
    case 0x08: /* LINE3D */
    case 0xC0:
    case 0xD3: case 0xD4: case 0xD5: case 0xD6:
    case 0xD7: /* TEXTURE */
    case 0xDD: /* LOAD_UCODE */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE6: case 0xE7: case 0xE8: case 0xE9:
    case 0xEA: case 0xEB: case 0xEC: case 0xEE: case 0xEF:
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5:
    case 0xF8: case 0xF9: case 0xFB: case 0xFC: case 0xFD: case 0xFE:
        break;
    case 0x01: { /* F3DEX2 G_VTX: n in bits 12-19, (v0+n) in bits 1-7 */
        int n = (int)((w0 >> 12) & 0xFF);
        int end = (int)((w0 >> 1) & 0x7F);
        int v0 = end - n;
        uint32_t base = 0;
        if (n <= 0 || n > 32 || v0 < 0 || !resolve_addr(w1, &base)) break;
        tz_gfx_vtx += (uint32_t)n;
        for (int i = 0; i < n; i++) xform(rdram, v0 + i, base + (uint32_t)(i * 16));
        break;
    }
    case 0xDA: { /* F3DEX2 G_MTX. Flags are in the low byte, XOR G_MTX_PUSH (1). */
        uint32_t param = (w0 & 0xFF) ^ 0x01u;
        int proj = param & 0x04;
        int load = param & 0x02;
        int push = param & 0x01;
        uint32_t base = 0;
        if (!resolve_addr(w1, &base)) break;
        float loaded[4][4];
        load_mtx(rdram, base, loaded);
        if (proj) {
            if (push && pj_sp + 1 < 4) {
                memcpy(pj_stack[pj_sp + 1], pj_stack[pj_sp], sizeof(loaded));
                pj_sp++;
            }
            if (load) memcpy(pj_stack[pj_sp], loaded, sizeof(loaded));
            else mul_mat(pj_stack[pj_sp], loaded, pj_stack[pj_sp]);
        } else {
            if (push && mv_sp + 1 < GFX_STACK) {
                memcpy(mv_stack[mv_sp + 1], mv_stack[mv_sp], sizeof(loaded));
                mv_sp++;
            }
            if (load) memcpy(mv_stack[mv_sp], loaded, sizeof(loaded));
            else mul_mat(mv_stack[mv_sp], loaded, mv_stack[mv_sp]);
        }
        mtx_proj = proj;
        break;
    }
    case 0xDB: { /* G_MOVEWORD. G_MW_SEGMENT = 6, offset = segment * 4. */
        int index = (int)((w0 >> 16) & 0xFF);
        int offset = (int)(w0 & 0xFFFF);
        if (index == 0x06) {
            int seg = offset / 4;
            if (seg >= 0 && seg < 16) seg_base[seg] = w1;
        }
        break;
    }
    case 0xDC: { /* G_MOVEMEM. F3DEX2 viewport index is 8 in the low byte. */
        if ((w0 & 0xFF) == 8) load_viewport(rdram, w1);
        break;
    }
    case 0xDE: { /* G_DL. Bit 16 set means branch, otherwise call. */
        uint32_t param = (w0 >> 16) & 0xFF;
        uint32_t dest = 0;
        if (!resolve_addr(w1, &dest)) {
            tz_gfx_unresolved++;
            break;
        }
        if (param & 1u) {
            *branch_to = (int)dest;
            *branched = 1;
        } else if (depth < 8) {
            run_dl(rdram, dest, depth + 1, 20000);
        }
        break;
    }
    case 0xD8: { /* G_POPMTX. w1 is count * 64. */
        int n = (int)(w1 / 64u);
        if (n < 1) n = 1;
        while (n-- > 0 && mv_sp > 0) mv_sp--;
        break;
    }
    case 0xD9: /* G_GEOMETRYMODE: keep bits in w0, set bits in w1 */
        geom_mode = (geom_mode & (int)(w0 & 0x00FFFFFFu)) | (int)w1;
        break;
    case 0xDF: /* ENDDL */
        *branched = 2;
        break;
    case 0x05: /* G_TRI1 indices live in w0 */
        draw_indexed(w0);
        break;
    case 0x06: /* G_TRI2 */
        draw_indexed(w0);
        draw_indexed(w1);
        break;
    case 0xE4: case 0xE5: { /* TEXRECT / TEXRECTFLIP, 10.2. Not a textured draw. */
        int x2 = (int)((w0 >> 12) & 0xFFF) >> 2;
        int y2 = (int)(w0 & 0xFFF) >> 2;
        int x1 = (int)((w1 >> 12) & 0xFFF) >> 2;
        int y1 = (int)(w1 & 0xFFF) >> 2;
        fill_rect(x1, y1, x2, y2, prim_color);
        tz_gfx_texrects++;
        break;
    }
    case 0xF6: { /* FILLRECT, 10.2 */
        int x2 = (int)((w0 >> 12) & 0xFFF) >> 2;
        int y2 = (int)(w0 & 0xFFF) >> 2;
        int x1 = (int)((w1 >> 12) & 0xFFF) >> 2;
        int y1 = (int)(w1 & 0xFFF) >> 2;
        fill_rect(x1, y1, x2, y2, fill_color);
        break;
    }
    case 0xF7: { /* SETFILLCOLOR RGBA5551 */
        uint16_t c = (uint16_t)(w1 & 0xFFFF);
        uint8_t r = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        uint8_t g = (uint8_t)(((c >> 6) & 31) * 255 / 31);
        uint8_t b = (uint8_t)(((c >> 1) & 31) * 255 / 31);
        fill_color = pack(r, g, b, 255);
        break;
    }
    case 0xFA: { /* SETPRIMCOLOR */
        prim_color = pack((uint8_t)((w1 >> 24) & 255), (uint8_t)((w1 >> 16) & 255),
                          (uint8_t)((w1 >> 8) & 255), (uint8_t)(w1 & 255));
        break;
    }
    case 0xFF: { /* SETCIMG */
        int width = (int)(w0 & 0xFFF) + 1;
        if (width >= 16 && width <= GFX_W) fb_w = width;
        if (fb_h < 16) fb_h = 240;
        break;
    }
    case 0xED: /* SETSCISSOR */ break;
    default:
        tz_gfx_unknown++;
        break;
    }
}

static void run_dl(uint8_t* rdram, uint32_t addr, int depth, int budget) {
    uint32_t resolved = 0;
    if (!resolve_addr(addr, &resolved)) {
        tz_gfx_unresolved++;
        return;
    }
    addr = resolved;
    for (int i = 0; i < budget; i++) {
        int branch_to = 0, branched = 0;
        one_cmd(rdram, addr, depth, &branch_to, &branched);
        if (branched == 2) return;
        if (branched == 1) {
            addr = (uint32_t)branch_to;
            continue;
        }
        addr += 8;
    }
}

void tz_gfx_reset(void) {
    fb_w = 320;
    fb_h = 240;
    mv_sp = 0;
    pj_sp = 0;
    ident(mv_stack[0]);
    ident(pj_stack[0]);
    memset(seg_base, 0, sizeof(seg_base));
    memset(pixels, 0, sizeof(pixels));
    drew = 0;
}

void tz_gfx_set_log(FILE* fp) { gfx_log = fp; }

int tz_gfx_has_frame(void) { return drew; }

void tz_gfx_dimensions(int* w, int* h) {
    *w = fb_w;
    *h = fb_h;
}

void tz_gfx_copy(uint32_t* dst, int max_pixels) {
    int n = fb_w * fb_h;
    if (n > max_pixels) n = max_pixels;
    if (n > 0) memcpy(dst, pixels, (size_t)n * sizeof(uint32_t));
}

void tz_gfx_task(uint8_t* rdram, uint32_t task) {
    uint32_t type = r32(rdram, task + 0x00);
    uint32_t data_ptr = r32(rdram, task + 0x30);
    uint32_t data_size = r32(rdram, task + 0x34);
    tz_gfx_tasks++;
    if (gfx_log) {
        fprintf(gfx_log, "TASK type=%u data=%08X size=%u ucode=%08X\n",
                type, data_ptr, data_size, r32(rdram, task + 0x10));
        fflush(gfx_log);
    }
    if (data_ptr == 0) return;
    uint32_t vtx0 = tz_gfx_vtx, tri0 = tz_gfx_triangles, fill0 = tz_gfx_fill_rects, miss0 = tz_gfx_unresolved;
    int budget = 20000;
    if (data_size >= 8 && data_size / 8 < (uint32_t)budget) budget = (int)(data_size / 8);
    run_dl(rdram, data_ptr, 0, budget);
    if (task_logs < 8) {
        task_logs++;
        fprintf(stderr, "GFX data=%08X size=%u vtx=%u tri=%u fill=%u unresolved=%u\n",
                data_ptr, data_size, tz_gfx_vtx - vtx0, tz_gfx_triangles - tri0,
                tz_gfx_fill_rects - fill0, tz_gfx_unresolved - miss0);
    }
}

void tz_gfx_dump_hist(FILE* fp) {
    if (!fp) return;
    for (int i = 0; i < 256; i++) {
        if (cmd_hist[i]) fprintf(fp, "OP %02X %u\n", i, cmd_hist[i]);
    }
}
