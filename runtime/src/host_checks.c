#include "tarzan_recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void expect(int cond, const char* msg) {
    if (cond) {
        printf("ok %s\n", msg);
        return;
    }
    fprintf(stderr, "FAIL %s\n", msg);
    fails++;
}

static int overlap(const TzZone* a, const TzZone* b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static const TzZone* find_btn(const TzZone* z, int n, uint16_t bit) {
    for (int i = 0; i < n; i++) {
        if (z[i].kind == TZ_ZONE_BTN && z[i].buttons == bit) return &z[i];
    }
    return NULL;
}

static void check_layout(int w, int h) {
    TzZone zones[16];
    int n = 0;
    tz_touch_layout(w, h, 96, zones, 16, &n);
    char label[64];
    snprintf(label, sizeof(label), "layout %dx%d count", w, h);
    expect(n >= 8, label);
    for (int i = 0; i < n; i++) {
        snprintf(label, sizeof(label), "%dx%d zone %d inside", w, h, i);
        expect(zones[i].x >= 0 && zones[i].y >= 0 && zones[i].x + zones[i].w <= w && zones[i].y + zones[i].h <= h, label);
        if (zones[i].kind == TZ_ZONE_BTN) {
            snprintf(label, sizeof(label), "%dx%d button %u target", w, h, zones[i].buttons);
            expect(zones[i].w >= 44 && zones[i].h >= 28, label);
        }
        for (int j = i + 1; j < n; j++) {
            snprintf(label, sizeof(label), "%dx%d overlap %d %d", w, h, i, j);
            expect(!overlap(&zones[i], &zones[j]), label);
        }
    }
    const TzZone* a = find_btn(zones, n, TZ_BTN_A);
    expect(a != NULL, "A zone exists");
    if (!a) return;
    int x = a->x + a->w / 2;
    int y = a->y + a->h / 2;
    uint16_t bits = 0;
    int8_t sx = 1, sy = 1;
    int active = 1;
    tz_touch_eval(zones, n, &x, &y, 1, &bits, &sx, &sy, &active);
    expect(bits == TZ_BTN_A && sx == 0 && sy == 0 && !active, "A hit is only A");
    const TzZone* dpad = NULL;
    const TzZone* stick = NULL;
    for (int i = 0; i < n; i++) {
        if (zones[i].kind == TZ_ZONE_DPAD) dpad = &zones[i];
        if (zones[i].kind == TZ_ZONE_STICK) stick = &zones[i];
    }
    expect(dpad && stick, "dpad and stick exist");
    if (dpad) {
        int ux = dpad->x + dpad->w / 2;
        int uy = dpad->y + dpad->h / 6;
        tz_touch_eval(zones, n, &ux, &uy, 1, &bits, &sx, &sy, &active);
        expect((bits & TZ_BTN_DUP) && sy > 0 && active, "dpad up");
    }
    if (stick) {
        int rx = stick->x + stick->w - 2;
        int ry = stick->y + stick->h / 2;
        tz_touch_eval(zones, n, &rx, &ry, 1, &bits, &sx, &sy, &active);
        expect(sx > 40 && active, "stick right");
    }
    x = 1;
    y = 1;
    tz_touch_eval(zones, n, &x, &y, 1, &bits, &sx, &sy, &active);
    expect(bits == 0 && sx == 0 && sy == 0 && !active, "miss is empty");
    tz_touch_eval(zones, n, NULL, NULL, 0, &bits, &sx, &sy, &active);
    expect(bits == 0 && !active, "no pointers clears");
}

int main(void) {
    uint8_t dig[32];
    const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t abc_dig[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    tz_sha256(abc, 3, dig);
    expect(memcmp(dig, abc_dig, 32) == 0, "sha256 abc");
    expect(!tz_rom_is_tarzan_us(abc, 3), "short buffer is not the ROM");
    uint8_t* fake = calloc(1, 64);
    expect(fake && !tz_rom_is_tarzan_us(fake, 64), "small zero buffer rejected");
    free(fake);

    const char* rom = ".work/rom/tarzan.z64";
    FILE* fp = fopen(rom, "rb");
    if (!fp) {
        fprintf(stderr, "FAIL local ROM missing for identity check\n");
        fails++;
    } else {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        uint8_t* buf = malloc((size_t)sz);
        expect(buf && fread(buf, 1, (size_t)sz, fp) == (size_t)sz, "read local ROM");
        fclose(fp);
        if (buf) {
            expect(tz_rom_is_tarzan_us(buf, (size_t)sz), "local ROM matches NTSC-U");
            buf[0] ^= 0xFF;
            expect(!tz_rom_is_tarzan_us(buf, (size_t)sz), "mutated ROM rejected");
            free(buf);
        }
    }

    char dir[] = "/tmp/tarzan-save-check-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/tarzan.eep", dir);
    uint8_t payload[512];
    for (int i = 0; i < 512; i++) payload[i] = (uint8_t)(i * 3 + 7);
    expect(tz_atomic_write(path, payload, 512) == 0, "atomic write");
    char tmp[140];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    expect(access(tmp, F_OK) != 0, "tmp removed after rename");
    fp = fopen(path, "rb");
    uint8_t got[512];
    expect(fp && fread(got, 1, 512, fp) == 512 && memcmp(got, payload, 512) == 0, "save bytes match");
    if (fp) fclose(fp);
    expect(tz_atomic_write("/no/such/dir/tarzan.eep", payload, 512) != 0, "missing dir fails");

    check_layout(1280, 720);
    check_layout(800, 480);
    TzZone tiny[4];
    int n = 99;
    tz_touch_layout(32, 32, 0, tiny, 4, &n);
    expect(n == 0, "tiny screen has no zones");

    if (fails) {
        fprintf(stderr, "%d checks failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
