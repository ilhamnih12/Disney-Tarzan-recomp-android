#include "tarzan_recomp.h"

static int add_zone(TzZone* out, int cap, int* n, int x, int y, int w, int h, uint16_t buttons, int kind) {
    if (*n >= cap || w < 8 || h < 8) return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    out[*n].x = x;
    out[*n].y = y;
    out[*n].w = w;
    out[*n].h = h;
    out[*n].buttons = buttons;
    out[*n].kind = kind;
    (*n)++;
    return 1;
}

void tz_touch_layout(int w, int h, int top_reserve, TzZone* out, int cap, int* count) {
    if (count) *count = 0;
    if (!out || !count || cap < 1 || w < 64 || h < 64) return;
    if (top_reserve < 0) top_reserve = 0;
    if (top_reserve > h / 2) top_reserve = h / 2;
    int unit = h / 8;
    if (unit > 84) unit = 84;
    if (unit < 48 && h >= 300) unit = 48;
    if (unit < 28) unit = 28;
    int margin = 12;
    int n = 0;
    int dpad = unit * 3;
    if (dpad > h - top_reserve - margin * 2) dpad = h - top_reserve - margin * 2;
    if (dpad < unit) dpad = unit;
    int dx = margin;
    int dy = h - margin - dpad;
    if (dy < top_reserve) dy = top_reserve;
    add_zone(out, cap, &n, dx, dy, dpad, dpad, 0, TZ_ZONE_DPAD);

    int stick = unit * 2;
    int sx = dx + dpad + margin;
    int sy = dy + (dpad - stick) / 2;
    if (sx + stick > w / 2) {
        sx = dx;
        sy = dy - margin - stick;
        if (sy < top_reserve) sy = top_reserve;
    }
    add_zone(out, cap, &n, sx, sy, stick, stick, 0, TZ_ZONE_STICK);

    int bw = unit;
    int bx = w - margin - bw;
    int by = h - margin - bw;
    if (by < top_reserve) by = top_reserve;
    add_zone(out, cap, &n, bx, by, bw, bw, TZ_BTN_A, TZ_ZONE_BTN);
    add_zone(out, cap, &n, bx - bw - margin, by, bw, bw, TZ_BTN_B, TZ_ZONE_BTN);
    add_zone(out, cap, &n, bx, by - bw - margin, bw, bw, TZ_BTN_Z, TZ_ZONE_BTN);
    int start_w = bw + 8;
    int start_x = w / 2 - start_w / 2;
    add_zone(out, cap, &n, start_x, by, start_w, bw / 2 > 28 ? bw / 2 : 28, TZ_BTN_START, TZ_ZONE_BTN);
    add_zone(out, cap, &n, margin, top_reserve + 4, bw, bw, TZ_BTN_L, TZ_ZONE_BTN);
    add_zone(out, cap, &n, w - margin - bw, top_reserve + 4, bw, bw, TZ_BTN_R, TZ_ZONE_BTN);

    int c = 48;
    int gap = 8;
    int cluster = c * 3 + gap;
    int b_left = bx - bw - margin;
    int right_limit = b_left - margin;
    int cx = right_limit - cluster;
    int cy = by + bw - cluster;
    if (cy < top_reserve) cy = top_reserve;
    if (cx > w / 2 && cy + cluster <= h - 4) {
        int mid_x = cx + c + gap / 2;
        int mid_y = cy + c + gap / 2;
        add_zone(out, cap, &n, mid_x, cy, c, c, TZ_BTN_CUP, TZ_ZONE_BTN);
        add_zone(out, cap, &n, mid_x, cy + 2 * c + gap, c, c, TZ_BTN_CDOWN, TZ_ZONE_BTN);
        add_zone(out, cap, &n, cx, mid_y, c, c, TZ_BTN_CLEFT, TZ_ZONE_BTN);
        add_zone(out, cap, &n, cx + 2 * c + gap, mid_y, c, c, TZ_BTN_CRIGHT, TZ_ZONE_BTN);
    }
    *count = n;
}

static int inside(const TzZone* z, int x, int y) {
    return x >= z->x && y >= z->y && x < z->x + z->w && y < z->y + z->h;
}

static int8_t clamp_stick(int v) {
    if (v > 80) return 80;
    if (v < -80) return -80;
    return (int8_t)v;
}

static void apply_dpad(const TzZone* z, int x, int y, uint16_t* buttons, int* sx, int* sy) {
    int lx = x - z->x;
    int ly = y - z->y;
    int tx = z->w / 3;
    int ty = z->h / 3;
    if (tx < 1) tx = 1;
    if (ty < 1) ty = 1;
    if (lx < tx) {
        *buttons |= TZ_BTN_DLEFT;
        *sx = -80;
    } else if (lx >= tx * 2) {
        *buttons |= TZ_BTN_DRIGHT;
        *sx = 80;
    }
    if (ly < ty) {
        *buttons |= TZ_BTN_DUP;
        *sy = 80;
    } else if (ly >= ty * 2) {
        *buttons |= TZ_BTN_DDOWN;
        *sy = -80;
    }
}

void tz_touch_eval(const TzZone* zones, int count, const int* xs, const int* ys, int npoints,
                   uint16_t* buttons, int8_t* stick_x, int8_t* stick_y, int* stick_active) {
    uint16_t bits = 0;
    int sx = 0, sy = 0;
    int have_stick = 0;
    int dsx = 0, dsy = 0;
    int have_dpad = 0;
    if (buttons) *buttons = 0;
    if (stick_x) *stick_x = 0;
    if (stick_y) *stick_y = 0;
    if (stick_active) *stick_active = 0;
    if (!zones || count <= 0 || !xs || !ys || npoints <= 0) return;
    for (int p = 0; p < npoints; p++) {
        for (int i = 0; i < count; i++) {
            if (!inside(&zones[i], xs[p], ys[p])) continue;
            if (zones[i].kind == TZ_ZONE_BTN) {
                bits |= zones[i].buttons;
            } else if (zones[i].kind == TZ_ZONE_DPAD) {
                apply_dpad(&zones[i], xs[p], ys[p], &bits, &dsx, &dsy);
                have_dpad = 1;
            } else if (zones[i].kind == TZ_ZONE_STICK) {
                int cx = zones[i].x + zones[i].w / 2;
                int cy = zones[i].y + zones[i].h / 2;
                int rx = zones[i].w / 2;
                int ry = zones[i].h / 2;
                if (rx < 1) rx = 1;
                if (ry < 1) ry = 1;
                sx = (xs[p] - cx) * 80 / rx;
                sy = (cy - ys[p]) * 80 / ry;
                have_stick = 1;
            }
        }
    }
    if (!have_stick && have_dpad) {
        sx = dsx;
        sy = dsy;
    }
    if (buttons) *buttons = bits;
    if (stick_x) *stick_x = clamp_stick(have_stick || have_dpad ? sx : 0);
    if (stick_y) *stick_y = clamp_stick(have_stick || have_dpad ? sy : 0);
    if (stick_active) *stick_active = have_stick || have_dpad;
}
