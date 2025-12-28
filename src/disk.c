#include <disk.h>

static int g_fd = -1;
static char g_img_path[256] = "disk.img";

static int _host_set_image(const char* path) {
    if (!path || !path[0]) return -1;
    snprintf(g_img_path, sizeof(g_img_path), "%s", path);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    return 0;
}

static int ensure_open() {
    if (g_fd >= 0) return 0;
    g_fd = open(g_img_path, O_RDWR);
    if (g_fd < 0) {
        fprintf(stderr, "[ATA] open('%s') failed: %s\n", g_img_path, strerror(errno));
        return -1;
    }

    return 1;
}

int DSK_host_open(const char* image_path) {
    _host_set_image(image_path);
    return ensure_open();
}

void DSK_host_close() {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}

static int full_pread(void* buf, size_t n, off_t off) {
    unsigned char* p = (unsigned char*)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(g_fd, p + done, n - done, off + (off_t)done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }

    return 0;
}

static int full_pwrite(const void* buf, size_t n, off_t off) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(g_fd, p + done, n - done, off + (off_t)done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }

    return 0;
}

unsigned char* DSK_read_sector(unsigned int lba) {
    return DSK_read_sectors(lba, 1);
}

unsigned char* DSK_read_sectors(unsigned int lba, unsigned int count) {
    if (ensure_open() != 0) return NULL;
    size_t bytes = (size_t)count * SECTOR_SIZE;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return NULL;

    off_t off = (off_t)lba * SECTOR_SIZE;
    if (full_pread(buf, bytes, off) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

unsigned char* DSK_readoff_sectors(unsigned int lba, unsigned int offset, unsigned int count) {
    if (ensure_open() != 0) return NULL;
    size_t bytes = (size_t)count * SECTOR_SIZE;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return NULL;

    off_t off = (off_t)lba * SECTOR_SIZE + (off_t)offset;
    if (full_pread(buf, bytes, off) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

unsigned char* DSK_readoff_sectors_stop(unsigned int lba, unsigned int offset, unsigned int count, unsigned char* stop) {
    if (stop) stop[0] = 0;
    return DSK_readoff_sectors(lba, offset, count);
}

int DSK_write_sectors(unsigned int lba, const unsigned char* data, unsigned int count) {
    if (ensure_open() != 0) return 0;
    size_t bytes = (size_t)count * SECTOR_SIZE;
    off_t off = (off_t)lba * SECTOR_SIZE;

    if (full_pwrite(data, bytes, off) != 0) return 0;
    return 1;
}

int DSK_writeoff_sectors(unsigned int lba, const unsigned char* data, unsigned int count, unsigned int offset, unsigned int size) {
    if (ensure_open() != 0) return 0;
    off_t off = (off_t)lba * SECTOR_SIZE + (off_t)offset;
    if (full_pwrite(data, (size_t)size, off) != 0) return 0;
    return 1;
}

int DSK_copy_sectors2sectors(unsigned int src_lba, unsigned int dst_lba, unsigned int count) {
    unsigned char* buf = DSK_read_sectors(src_lba, count);
    if (!buf) return 0;
    int ok = DSK_write_sectors(dst_lba, buf, count);
    free(buf);
    return ok;
}
