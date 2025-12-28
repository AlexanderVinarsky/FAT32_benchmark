// #define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <disk.h>
#include <fat.h>

typedef struct {
    uint64_t total_us;
    uint64_t count;
    uint64_t min_us;
    uint64_t max_us;
} timer_us_t;

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void tadd(timer_us_t* t, uint64_t us) {
    t->total_us += us;
    t->count++;
    if (t->count == 1) { t->min_us = us; t->max_us = us; }
    else { if (us < t->min_us) t->min_us = us; if (us > t->max_us) t->max_us = us; }
}

static double tavg(const timer_us_t* t) {
    return (t->count == 0) ? 0.0 : (double)t->total_us / (double)t->count;
}

#define MEASURE_US(code) ({ uint64_t _a = now_us(); code; uint64_t _b = now_us(); (_b - _a); })

typedef struct {
    char  val1[128];
    char  val2;
    short val3;
    int   val4;
    long  val5;
} __attribute__((packed)) test_val_t;

static char* filenames[] = {
    "root/test.txt",   "root/tdir/asd.bin", "root/dir/dir2/dir3/123.dr", "root/terrr/terrr",
    "root/test1.txt",  "root/test2.txt",    "root/asd",                  "root/rt/fr/or/qwe/df/wd/lf/ls/ge/w/e/r/t/y/u/i/file.txt",
    "root/test_1.txt", "root/test_2.txt",   "root/test_3.txt",           "root/test_4.txt", "root/test_5.txt", "root/test_6.txt",
    "root/test_7.txt", "root/test_8.txt",   "root/test_9.txt",           "root/test_10.txt"
};

static int g_id = 1;
static const char* get_name(char* buf12) {
    snprintf(buf12, 12, "%06d.pg", g_id++);
    return buf12;
}

static void norm_path(const char* in, char* out, size_t out_sz) {
    size_t n = strlen(in);
    if (n >= out_sz) n = out_sz - 1;
    for (size_t i = 0; i < n; i++) out[i] = (in[i] == '/') ? '\\' : in[i];
    out[n] = 0;
}

static void split_parent_file(const char* full, char* parent, size_t psz, char* file, size_t fsz) {
    const char* last = strrchr(full, '\\');
    if (!last) {
        parent[0] = 0;
        strncpy(file, full, fsz - 1);
        file[fsz - 1] = 0;
        return;
    }

    size_t plen = (size_t)(last - full);
    if (plen >= psz) plen = psz - 1;
    memcpy(parent, full, plen);
    parent[plen] = 0;

    strncpy(file, last + 1, fsz - 1);
    file[fsz - 1] = 0;
}

static void split_name_ext(const char* fname, char* name, size_t nsz, char* ext, size_t esz) {
    const char* dot = strrchr(fname, '.');
    if (!dot || dot == fname) {
        strncpy(name, fname, nsz - 1);
        name[nsz - 1] = 0;
        ext[0] = 0;
        return;
    }

    size_t ln = (size_t)(dot - fname);
    if (ln >= nsz) ln = nsz - 1;
    memcpy(name, fname, ln);
    name[ln] = 0;

    strncpy(ext, dot + 1, esz - 1);
    ext[esz - 1] = 0;
}

static void join_path(char* out, size_t osz, const char* parent, const char* fname) {
    out[0] = 0;
    strncat(out, parent, osz - 1);
    if (strlen(out) + 1 < osz) strncat(out, "\\", osz - 1 - strlen(out));
    strncat(out, fname, osz - 1 - strlen(out));
}

// timers
static timer_us_t open_t, create_t, write_t, read_t, rename_t, copy_t, delete_t;

int main(int argc, char** argv) {
#ifndef NO_CREATION
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <count> <img_path>\n", argv[0]);
        return 1;
    }

    int count = atoi(argv[1]);
    if (!DSK_host_open(argv[2])) {
        fprintf(stderr, "DSK_host_open(disk.img) failed\n");
        return 1;
    }

    if (FAT_initialize() != 0) {
        fprintf(stderr, "FAT_initialize() failed (is %s FAT32?)\n", argv[2]);
        DSK_host_close();
        return 1;
    }

    printf(
        "fat_type=%u bps=%u spc=%u total_clusters=%u root_cluster=%u first_fat=%u first_data=%u fat_size=%u\n",
        FAT_data.fat_type, FAT_data.bytes_per_sector, FAT_data.sectors_per_cluster,
        FAT_data.total_clusters, FAT_data.ext_root_cluster,
        FAT_data.first_fat_sector, FAT_data.first_data_sector, FAT_data.fat_size
    );

    if (FAT_content_exists("root") != 1) {
        fprintf(stderr, "Directory 'root' not found in image. Create it in %s.\n", argv[2]);
        DSK_host_close();
        return 1;
    }

    int handled_errors = 0;
    int unhandled_errors = 0;

    test_val_t data = { .val2 = 0x1A, .val3 = 0x0F34, .val4 = 0x0DEA, .val5 = 0xDEAD };
    memset(data.val1, 0, sizeof(data.val1));
    strncpy(data.val1, "Test data from structure! Hello there from structure, I guess..", sizeof(data.val1));

    unsigned char rbuf[sizeof(test_val_t)] = {0};
    unsigned int offset = 0;

    while (count-- > 0) {
        char target[512] = {0};
        norm_path(filenames[count % 18], target, sizeof(target));

        char parent[512] = {0};
        char fname[128] = {0};
        split_parent_file(target, parent, sizeof(parent), fname, sizeof(fname));

        char base[64] = {0};
        char ext[16] = {0};
        split_name_ext(fname, base, sizeof(base), ext, sizeof(ext));

        int put_rc = -1;
        tadd(&create_t, MEASURE_US({
            Content* obj = FAT_create_object(base, 0, ext); 
            if (!obj) { put_rc = -1; }
            else {
                fprintf(stderr, "[DBG] create: parent='%s' base='%s' ext='%s' full='%s'\n", parent, base, ext, target);
                put_rc = FAT_put_content(parent, obj);
                FAT_unload_content_system(obj);
            }
        }));

        if (put_rc != 0) { unhandled_errors++; continue; }

        int ci = -1;
        tadd(&open_t, MEASURE_US({
            fprintf(stderr, "[DBG] open: '%s'\n", target);
            ci = FAT_open_content(target);
        }));

        if (ci < 0) { unhandled_errors++; continue; }

        int wrc = 0;
        tadd(&write_t, MEASURE_US({
            wrc = FAT_write_buffer2content(ci, (const unsigned char*)&data, offset, (unsigned int)sizeof(test_val_t));
        }));

        if (wrc < 0) { FAT_close_content(ci); unhandled_errors++; continue; }

        int rrc = 0;
        tadd(&read_t, MEASURE_US({
            memset(rbuf, 0, sizeof(rbuf));
            rrc = FAT_read_content2buffer(ci, rbuf, offset, (unsigned int)sizeof(test_val_t));
        }));

        if (rrc != (int)sizeof(test_val_t) || memcmp(rbuf, &data, sizeof(test_val_t)) != 0) {
            handled_errors++;
        }

        char new_dot[12] = {0};
        get_name(new_dot);

        char new_fat[13] = {0};
        strncpy(new_fat, new_dot, sizeof(new_fat) - 1);
        _name2fatname(new_fat);

        int rnm = -1;
        tadd(&rename_t, MEASURE_US({
            rnm = FAT_change_meta(target, new_fat);
        }));

        if (rnm != 0) { FAT_close_content(ci); unhandled_errors++; continue; }

        char src_path[512] = {0};
        join_path(src_path, sizeof(src_path), parent, new_dot);

        char dst_dot[12] = {0};
        get_name(dst_dot);

        char dst_base[64] = {0};
        char dst_ext[16] = {0};
        split_name_ext(dst_dot, dst_base, sizeof(dst_base), dst_ext, sizeof(dst_ext));

        char dst_path[512] = {0};
        join_path(dst_path, sizeof(dst_path), parent, dst_dot);

        int put2 = -1;
        tadd(&create_t, MEASURE_US({
            Content* dst_obj = FAT_create_object(dst_base, 0, dst_ext);
            if (!dst_obj) put2 = -1;
            else {
                fprintf(stderr, "[DBG] create: parent='%s' base='%s' ext='%s' full='%s'\n", parent, base, ext, target);
                put2 = FAT_put_content(parent, dst_obj);
                FAT_unload_content_system(dst_obj);
            }
        }));

        if (put2 != 0) { FAT_close_content(ci); unhandled_errors++; continue; }

        int dst_ci = -1;
        tadd(&open_t, MEASURE_US({
            fprintf(stderr, "[DBG] open: '%s'\n", target);
            dst_ci = FAT_open_content(dst_path);
        }));

        if (dst_ci < 0) { FAT_close_content(ci); unhandled_errors++; continue; }

        tadd(&copy_t, MEASURE_US({
            unsigned char tmp[sizeof(test_val_t)] = {0};
            FAT_read_content2buffer(ci, tmp, offset, (unsigned int)sizeof(test_val_t));
            FAT_write_buffer2content(dst_ci, tmp, offset, (unsigned int)sizeof(test_val_t));
        }));

        tadd(&read_t, MEASURE_US({
            memset(rbuf, 0, sizeof(rbuf));
            FAT_read_content2buffer(dst_ci, rbuf, offset, (unsigned int)sizeof(test_val_t));
        }));
        
        if (memcmp(rbuf, &data, sizeof(test_val_t)) != 0) handled_errors++;

        FAT_close_content(dst_ci);
        FAT_close_content(ci);

#ifdef DO_DELETE
        tadd(&delete_t, MEASURE_US({
            FAT_delete_content(dst_path);
            FAT_delete_content(src_path);
        }));
#endif
    }

    time_t now = time(NULL);
    char time_str[32] = {0};
    struct tm* tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(stdout, "[%s] Handled errors: %d\tUnhandled errors: %d\n", time_str, handled_errors, unhandled_errors);
    fprintf(stdout, "Total generated names: %d\n", g_id - 1);

    fprintf(stdout, "\n==== Performance Summary ====\n");
    fprintf(stdout, "Avg open time:   %.2f us\n", tavg(&open_t));
    fprintf(stdout, "Avg create time: %.2f us\n", tavg(&create_t));
    fprintf(stdout, "Avg write time:  %.2f us\n", tavg(&write_t));
    fprintf(stdout, "Avg read time:   %.2f us\n", tavg(&read_t));
    fprintf(stdout, "Avg rename time: %.2f us\n", tavg(&rename_t));
    fprintf(stdout, "Avg copy time:   %.2f us\n", tavg(&copy_t));
    fprintf(stdout, "Avg delete time: %.2f us\n", tavg(&delete_t));
    fprintf(stdout, "=============================\n\n");

    DSK_host_close();
#endif
    return 0;
}
