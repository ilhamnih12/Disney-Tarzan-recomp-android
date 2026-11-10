#include "tarzan_recomp.h"
#include "android_native_app_glue.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * NativeActivity shell. The ROM is not packaged. There is no Play control.
 * Touch zones map an N64 pad for an experimental boot only. Losing focus
 * clears every button so a press cannot stick.
 */

#define LOG_TAG "tarzan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char k_font_index[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:/-,";
static const uint8_t k_font[][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F},
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F},
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},
};

enum { ROM_MISSING = 0, ROM_BAD, ROM_OK };

static uint16_t g_key_buttons, g_touch_buttons;
static int8_t g_key_sx, g_key_sy, g_touch_sx, g_touch_sy;
static int g_touch_stick;
static int g_focused = 1;
static int g_have_window;
static int g_booted;
static int g_layout_w, g_layout_h, g_layout_top;

static int font_row(char ch, int row) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    const char* hit = strchr(k_font_index, ch);
    if (!hit || row < 0 || row > 6) return 0;
    return k_font[(int)(hit - k_font_index)][row];
}

static void put_px(uint32_t* pix, int stride, int w, int h, int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    pix[y * stride + x] = color;
}

static void draw_char(uint32_t* pix, int stride, int w, int h, int x, int y, char ch, uint32_t color, int scale) {
    for (int row = 0; row < 7; row++) {
        int bits = font_row(ch, row);
        for (int col = 0; col < 5; col++) {
            if ((bits & (0x10 >> col)) == 0) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    put_px(pix, stride, w, h, x + col * scale + sx, y + row * scale + sy, color);
                }
            }
        }
    }
}

static void draw_text(uint32_t* pix, int stride, int w, int h, int x, int y, const char* text, uint32_t color, int scale) {
    int cx = x;
    for (const char* p = text; *p; p++) {
        draw_char(pix, stride, w, h, cx, y, *p, color, scale);
        cx += 6 * scale;
        if (cx + 6 * scale > w) break;
    }
}

static void draw_box(uint32_t* pix, int stride, int w, int h, int x, int y, int bw, int bh, uint32_t color) {
    for (int yy = y; yy < y + bh; yy++) {
        for (int xx = x; xx < x + bw; xx++) put_px(pix, stride, w, h, xx, yy, color);
    }
}

static void push_input(void) {
    if (!g_booted) return;
    if (!g_focused) {
        tz_set_input(0, 0, 0);
        return;
    }
    int8_t sx = g_touch_stick ? g_touch_sx : g_key_sx;
    int8_t sy = g_touch_stick ? g_touch_sy : g_key_sy;
    tz_set_input((uint16_t)(g_key_buttons | g_touch_buttons), sx, sy);
}

static void clear_keys(void) {
    g_key_buttons = 0;
    g_key_sx = 0;
    g_key_sy = 0;
}

static void clear_touch(void) {
    g_touch_buttons = 0;
    g_touch_sx = 0;
    g_touch_sy = 0;
    g_touch_stick = 0;
}

static void release_input(void) {
    clear_keys();
    clear_touch();
    push_input();
}

static void note_key(int code, int down) {
    int bit = 0;
    int axis = 0;
    int8_t axis_val = 0;
    switch (code) {
    case AKEYCODE_DPAD_UP: bit = TZ_BTN_DUP; axis = 2; axis_val = 80; break;
    case AKEYCODE_DPAD_DOWN: bit = TZ_BTN_DDOWN; axis = 2; axis_val = -80; break;
    case AKEYCODE_DPAD_LEFT: bit = TZ_BTN_DLEFT; axis = 1; axis_val = -80; break;
    case AKEYCODE_DPAD_RIGHT: bit = TZ_BTN_DRIGHT; axis = 1; axis_val = 80; break;
    case AKEYCODE_BUTTON_A:
    case AKEYCODE_ENTER: bit = TZ_BTN_A; break;
    case AKEYCODE_BUTTON_B:
    case AKEYCODE_DEL: bit = TZ_BTN_B; break;
    case AKEYCODE_BUTTON_START:
    case AKEYCODE_SPACE: bit = TZ_BTN_START; break;
    case AKEYCODE_BUTTON_L1: bit = TZ_BTN_L; break;
    case AKEYCODE_BUTTON_R1: bit = TZ_BTN_R; break;
    case AKEYCODE_BUTTON_Z: bit = TZ_BTN_Z; break;
    case AKEYCODE_BUTTON_C: bit = TZ_BTN_CUP; break;
    default: return;
    }
    if (down) g_key_buttons = (uint16_t)(g_key_buttons | bit);
    else g_key_buttons = (uint16_t)(g_key_buttons & ~bit);
    if (axis == 1) g_key_sx = down ? axis_val : 0;
    if (axis == 2) g_key_sy = down ? axis_val : 0;
}

static void apply_motion(struct android_app* app, AInputEvent* event) {
    int action = AMotionEvent_getAction(event);
    int masked = action & AMOTION_EVENT_ACTION_MASK;
    if (masked == AMOTION_EVENT_ACTION_CANCEL || masked == AMOTION_EVENT_ACTION_UP) {
        clear_touch();
        push_input();
        return;
    }
    int skip = -1;
    if (masked == AMOTION_EVENT_ACTION_POINTER_UP) {
        skip = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    }
    int w = g_layout_w;
    int h = g_layout_h;
    int top = g_layout_top;
    if ((w < 16 || h < 16) && app->window) {
        w = ANativeWindow_getWidth(app->window);
        h = ANativeWindow_getHeight(app->window);
        top = h / 3;
    }
    TzZone zones[16];
    int nz = 0;
    tz_touch_layout(w, h, top, zones, 16, &nz);
    int xs[12], ys[12], n = 0;
    size_t count = AMotionEvent_getPointerCount(event);
    for (size_t i = 0; i < count && n < 12; i++) {
        if ((int)i == skip) continue;
        xs[n] = (int)AMotionEvent_getX(event, i);
        ys[n] = (int)AMotionEvent_getY(event, i);
        n++;
    }
    tz_touch_eval(zones, nz, xs, ys, n, &g_touch_buttons, &g_touch_sx, &g_touch_sy, &g_touch_stick);
    push_input();
}

static int32_t on_input(struct android_app* app, AInputEvent* event) {
    int type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        apply_motion(app, event);
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_KEY) return 0;
    int action = AKeyEvent_getAction(event);
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 0;
    note_key(AKeyEvent_getKeyCode(event), action == AKEY_EVENT_ACTION_DOWN);
    push_input();
    return 1;
}

static void on_cmd(struct android_app* app, int32_t cmd) {
    (void)app;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        g_have_window = 1;
        if (g_booted && g_focused) tz_set_active(1);
        break;
    case APP_CMD_TERM_WINDOW:
        g_have_window = 0;
        release_input();
        tz_set_active(0);
        break;
    case APP_CMD_GAINED_FOCUS:
        g_focused = 1;
        if (g_booted) tz_set_active(1);
        break;
    case APP_CMD_LOST_FOCUS:
    case APP_CMD_PAUSE:
        g_focused = 0;
        release_input();
        tz_set_active(0);
        LOGI("fokus hilang, input direset");
        break;
    case APP_CMD_RESUME:
        g_focused = 1;
        if (g_booted) tz_set_active(1);
        break;
    default:
        break;
    }
}

static int load_matching_rom(const char* path, uint8_t** out, size_t* out_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return ROM_MISSING;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ROM_BAD;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        fclose(fp);
        return ROM_BAD;
    }
    rewind(fp);
    uint8_t* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return ROM_BAD;
    }
    fclose(fp);
    if (!tz_rom_is_tarzan_us(buf, (size_t)sz)) {
        free(buf);
        return ROM_BAD;
    }
    *out = buf;
    *out_size = (size_t)sz;
    return ROM_OK;
}

static void blit_frame(uint32_t* dst, int stride, int w, int h, int x0, int y0, int dw, int dh) {
    int fw = tz_frame_width();
    int fh = tz_frame_height();
    if (fw < 1 || fh < 1 || dw < 1 || dh < 1) return;
    uint32_t* src = calloc((size_t)fw * (size_t)fh, sizeof(uint32_t));
    if (!src) return;
    tz_copy_frame(src, fw * fh);
    for (int y = 0; y < dh; y++) {
        int sy = y * fh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * fw / dw;
            put_px(dst, stride, w, h, x0 + x, y0 + y, src[sy * fw + sx] | 0xFF000000u);
        }
    }
    free(src);
}

static const char* zone_label(const TzZone* z) {
    if (z->kind == TZ_ZONE_STICK) return "STICK";
    if (z->kind == TZ_ZONE_DPAD) return "DPAD";
    switch (z->buttons) {
    case TZ_BTN_A: return "A";
    case TZ_BTN_B: return "B";
    case TZ_BTN_Z: return "Z";
    case TZ_BTN_START: return "S";
    case TZ_BTN_L: return "L";
    case TZ_BTN_R: return "R";
    case TZ_BTN_CUP: return "CU";
    case TZ_BTN_CDOWN: return "CD";
    case TZ_BTN_CLEFT: return "CL";
    case TZ_BTN_CRIGHT: return "CR";
    default: return "";
    }
}

static void paint(struct android_app* app, const char* rom_path, int rom_state) {
    if (!app->window || !g_have_window) return;
    int32_t w = ANativeWindow_getWidth(app->window);
    int32_t h = ANativeWindow_getHeight(app->window);
    if (w < 16 || h < 16) return;
    if (ANativeWindow_setBuffersGeometry(app->window, w, h, WINDOW_FORMAT_RGBA_8888) != 0) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(app->window, &buf, NULL) != 0) return;
    uint32_t* pix = buf.bits;
    int stride = buf.stride;
    for (int y = 0; y < buf.height; y++) {
        for (int x = 0; x < buf.width; x++) pix[y * stride + x] = 0xFF102418u;
    }
    int scale = buf.width / 320;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    int y = 8;
    uint32_t amber = 0xFF3DB8FFu;
    uint32_t white = 0xFFF2F6F3u;
    uint32_t red = 0xFF4040FFu;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "TARZAN RECOMP", amber, scale);
    y += 12 * scale;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "BELUM BISA DIMAINKAN", red, scale);
    y += 12 * scale;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "ROM TIDAK ADA DI APLIKASI", white, scale);
    y += 10 * scale;
    if (rom_state == ROM_BAD) draw_text(pix, stride, buf.width, buf.height, 8, y, "ROM BUKAN TARZAN NTSC-U", red, scale);
    else if (rom_state == ROM_OK) draw_text(pix, stride, buf.width, buf.height, 8, y, "BOOT EKSPERIMEN, BUKAN SIAP MAIN", amber, scale);
    else draw_text(pix, stride, buf.width, buf.height, 8, y, "LETAKKAN ROM NTSC-U MILIK ANDA", white, scale);
    y += 10 * scale;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "HLE F3DEX2, BUKAN RT64/VULKAN", white, scale);
    y += 10 * scale;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "KONTROL BUKAN TOMBOL MAIN", red, scale);
    y += 10 * scale;
    draw_text(pix, stride, buf.width, buf.height, 8, y, "FOKUS HILANG: INPUT DINOLKAN", white, scale);
    y += 12 * scale;
    if (rom_path) draw_text(pix, stride, buf.width, buf.height, 8, y, rom_path, amber, 1);
    y += 12;
    g_layout_w = buf.width;
    g_layout_h = buf.height;
    g_layout_top = y;
    TzZone zones[16];
    int nz = 0;
    tz_touch_layout(buf.width, buf.height, y, zones, 16, &nz);
    for (int i = 0; i < nz; i++) {
        draw_box(pix, stride, buf.width, buf.height, zones[i].x, zones[i].y, zones[i].w, zones[i].h, 0xFF1C3A28u);
        draw_text(pix, stride, buf.width, buf.height, zones[i].x + 4, zones[i].y + 4, zone_label(&zones[i]), white, 1);
    }
    if (g_booted && y + 48 < buf.height) {
        int frame_x = buf.width / 3;
        int dw = buf.width / 3;
        int dh = dw * 3 / 4;
        int limit = buf.height - (84 * 3);
        if (limit < y + 16) limit = y + 16;
        if (y + dh > limit) dh = limit - y;
        if (dw > 16 && dh > 16) blit_frame(pix, stride, buf.width, buf.height, frame_x, y, dw, dh);
    }
    ANativeWindow_unlockAndPost(app->window);
}

void android_main(struct android_app* app) {
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    const char* dir = app->activity ? app->activity->internalDataPath : NULL;
    char rom_path[512];
    rom_path[0] = 0;
    if (dir && dir[0]) snprintf(rom_path, sizeof(rom_path), "%s/tarzan.z64", dir);
    else snprintf(rom_path, sizeof(rom_path), "internalDataPath kosong");
    LOGI("rom path %s", rom_path);

    uint8_t* rom = NULL;
    size_t rom_size = 0;
    int rom_state = ROM_MISSING;
    if (dir && dir[0]) rom_state = load_matching_rom(rom_path, &rom, &rom_size);
    if (rom_state == ROM_OK) {
        if (tz_init(rom, rom_size, dir) == 0) {
            tz_start();
            g_booted = 1;
            LOGI("experimental boot started");
        } else {
            rom_state = ROM_BAD;
            LOGE("tz_init failed");
        }
        free(rom);
    }

    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source* source = NULL;
        int ident = ALooper_pollOnce(g_booted ? 8 : 50, NULL, &events, (void**)&source);
        if (ident >= 0 && source) source->process(app, source);
        if (g_have_window) paint(app, rom_path, rom_state);
    }
    release_input();
    if (g_booted) tz_shutdown();
}
