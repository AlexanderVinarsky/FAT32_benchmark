#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "disk.h"
#include "fat.h"

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

#define MEASURE_US(expr) ({ uint64_t _a = now_us(); expr; uint64_t _b = now_us(); (_b - _a); })

static void make_name(char* out, size_t n, unsigned int i) {
    snprintf(out, n, "f%07u", i);
}

static void fill_pattern(unsigned char* p, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        p[i] = (unsigned char)(seed >> 24);
    }
}

static int verify_pattern(const unsigned char* p, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        if (p[i] != (unsigned char)(seed >> 24)) return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <N_files> <rw_mb> <img>\n", argv[0]);
        return 1;
    }

    unsigned int N = (unsigned int)atoi(argv[1]);
    unsigned int RW_MB = (unsigned int)atoi(argv[2]);
    const char* img = argv[3];
    if (!DSK_host_open(img)) return 1;
    printf("N=%u, RW_MB=%u, img=%s\n", N, RW_MB, img);

    uint64_t t_init = MEASURE_US({
        if (FAT_initialize() != 0) {
            fprintf(stderr, "FAT_initialize failed\n");
            exit(1);
        }
    });

    if (!FAT_content_exists("ROOT/BENCH")) {
        fprintf(stdout, "Creating bench directory...\n");

        Content* d = FAT_create_object("BENCH", 1, "");
        int sput_res = FAT_put_content("ROOT", d);
        FAT_unload_content_system(d);

        fprintf(stdout, "Put results: %i\n", sput_res);
    }

    if (!FAT_content_exists("ROOT/BENCH")) {
        fprintf(stderr, "Bench directory hasn't created!\n");
        return 1;
    }

    uint64_t t_create = 0;
    for (unsigned int i = 0; i < N; i++) {
        char name[32];
        make_name(name, sizeof(name), i);
        t_create += MEASURE_US({
            Content* o = FAT_create_object(name, 0, "bin");
            FAT_put_content("ROOT/BENCH", o);
            FAT_unload_content_system(o);
        });
    }

    int ci = FAT_open_content("ROOT/BENCH/f0000000.bin");
    if (ci < 0) {
        fprintf(stderr, "Can't find a test rw file!\n");
        return 1;
    }

    const size_t chunk = 4096;
    const size_t total_bytes = (size_t)RW_MB * 1024 * 1024;
    unsigned char* buf = malloc(chunk);

    uint64_t t_append = 0;
    size_t off = 0;
    uint32_t seed = 0x12345678;

    while (off < total_bytes) {
        size_t n = (total_bytes - off > chunk) ? chunk : (total_bytes - off);
        fill_pattern(buf, n, seed);

        t_append += MEASURE_US({
            FAT_write_buffer2content(ci, buf, off, (unsigned int)n);
        });

        off += n;
    }

    unsigned char* rbuf = malloc(chunk);
    uint64_t t_read = 0;
    off = 0;
    seed = 0x12345678;

    while (off < total_bytes) {
        size_t n = (total_bytes - off > chunk) ? chunk : (total_bytes - off);
        t_read += MEASURE_US({
            FAT_read_content2buffer(ci, rbuf, off, (unsigned int)n);
        });

        if (verify_pattern(rbuf, n, seed) != 0) {
            fprintf(stderr, "verify failed at offset %zu\n", off);
            break;
        }

        off += n;
    }

    FAT_close_content(ci);
    free(buf);
    free(rbuf);
    DSK_host_close();

    printf("\n==== FS BENCH ====\n");
    printf("init:          %8.6f ms\n", (double)t_init / 1000.0);
    printf("create %u:     %8.6f ms (%.2f us/op)\n", N, (double)t_create / 1000.0, (double)t_create / (double)N);
    printf("append %u MB:  %8.6f ms (%.2f MB/s)\n",
           RW_MB,
           (double)t_append / 1000.0,
           (double)RW_MB / ((double)t_append / 1000000.0));
    printf("read %u MB:    %8.6f ms (%.2f MB/s)\n",
           RW_MB,
           (double)t_read / 1000.0,
           (double)RW_MB / ((double)t_read / 1000000.0));
    printf("==================\n");

    return 0;
}