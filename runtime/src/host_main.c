#include "tarzan_recomp.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void on_crash(int sig, siginfo_t* info, void* uctx) {
    (void)uctx;
    fprintf(stderr, "signal %d fault=%p\n", sig, info ? info->si_addr : NULL);
    void* frames[48];
    int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    _exit(128 + sig);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int write_ppm(const char* path, const uint32_t* pix, int w, int h) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t p = pix[i];
        uint8_t rgb[3] = {(uint8_t)(p & 255), (uint8_t)((p >> 8) & 255), (uint8_t)((p >> 16) & 255)};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s --rom <file.z64> [--seconds N] [--dump frame.ppm] [--log boot.log] [--save-dir DIR]\n",
            argv0);
}

int main(int argc, char** argv) {
    const char* rom_path = NULL;
    const char* dump_path = "artifacts/host-frame.ppm";
    const char* log_path = "artifacts/host-boot.log";
    const char* save_dir = "artifacts";
    int seconds = 8;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom_path = argv[++i];
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump_path = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) log_path = argv[++i];
        else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) save_dir = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!rom_path) {
        usage(argv[0]);
        return 2;
    }
    FILE* romf = fopen(rom_path, "rb");
    if (!romf) {
        perror(rom_path);
        return 1;
    }
    fseek(romf, 0, SEEK_END);
    long rom_size = ftell(romf);
    fseek(romf, 0, SEEK_SET);
    if (rom_size < 0x1000 || rom_size > 64 * 1024 * 1024) {
        fprintf(stderr, "ROM size %ld is not a Tarzan image\n", rom_size);
        fclose(romf);
        return 1;
    }
    uint8_t* rom = malloc((size_t)rom_size);
    if (!rom || fread(rom, 1, (size_t)rom_size, romf) != (size_t)rom_size) {
        fprintf(stderr, "failed to read ROM\n");
        fclose(romf);
        free(rom);
        return 1;
    }
    fclose(romf);
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        fprintf(stderr, "ROM is not big-endian .z64\n");
        free(rom);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    FILE* log = fopen(log_path, "w");
    if (log) tz_set_log(log);
    if (tz_init(rom, (size_t)rom_size, save_dir) != 0) {
        fprintf(stderr, "tz_init failed\n");
        free(rom);
        return 1;
    }
    free(rom);
    printf("host start, running %d s quit=%d\n", seconds, tz_should_quit());
    tz_start();
    uint64_t t0 = now_ms();
    int last_serial = -1;
    int frames_seen = 0;
    while ((int)((now_ms() - t0) / 1000ull) < seconds && !tz_should_quit()) {
        int serial = tz_frame_serial();
        if (serial != last_serial) {
            last_serial = serial;
            frames_seen++;
        }
        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
    TzStats stats;
    tz_get_stats(&stats);
    int w = tz_frame_width();
    int h = tz_frame_height();
    uint32_t* pix = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    int dumped = -1;
    if (pix && w > 0 && h > 0) {
        tz_copy_frame(pix, w * h);
        dumped = write_ppm(dump_path, pix, w, h);
    }
    printf("vi=%u gfx=%u audio=%u tris=%u fills=%u vtx=%u tex=%u unresolved=%u dma=%u unknown_cmd=%u threads=%u unimpl=%u frames_seen=%d dumped=%d %dx%d\n",
           stats.vi_frames, stats.gfx_tasks, stats.audio_tasks, stats.triangles, stats.fill_rects,
           stats.vtx, stats.tex_rects, stats.unresolved_dl,
           stats.dma_bytes, stats.unknown_cmds, stats.threads_started, stats.unimplemented,
           frames_seen, dumped, w, h);
    tz_dump_gfx_hist(stderr);
    tz_shutdown();
    if (log) fclose(log);
    free(pix);
    return 0;
}
