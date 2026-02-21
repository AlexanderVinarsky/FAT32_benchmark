#include "disk.h"

static int  g_fd = -1;
static char g_img_path[1024] = "disk.img";

static int _host_set_image(const char* path) {
    if (!path || !path[0]) return 0;
    snprintf(g_img_path, sizeof(g_img_path), "%s", path);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    return 1;
}

static int ensure_open() {
    if (g_fd >= 0) return 1;

    int flags = O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    g_fd = open(g_img_path, flags);
    if (g_fd < 0) {
        fprintf(stderr, "[DSK] open('%s') failed: %s\n", g_img_path, strerror(errno));
        return 0;
    }

    return 1;
}

int DSK_host_open(const char* image_path) {
    if (!_host_set_image(image_path)) return 0;
    return ensure_open();
}

void DSK_host_close(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}

static int full_pread(void* buf, size_t n, off_t off) {
    unsigned char* p = (unsigned char*)buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = pread(g_fd, p + done, n - done, off + (off_t)done);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r == 0) return -1;        
        if (errno == EINTR) continue;   
        return -1;
    }
    return 0;
}

static int full_pwrite(const void* buf, size_t n, off_t off) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = pwrite(g_fd, p + done, n - done, off + (off_t)done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (errno == EINTR) continue;  
        return -1;
    }
    return 0;
}

int DSK_read_sectors_into(unsigned int lba, unsigned int count, unsigned char* out) {
    if (!out) return 0;
    if (!ensure_open()) return 0;

    size_t bytes = (size_t)count * SECTOR_SIZE;
    off_t off = (off_t)((uint64_t)lba * (uint64_t)SECTOR_SIZE);

    return (full_pread(out, bytes, off) == 0) ? 1 : 0;
}

int DSK_readoff_sectors_into(unsigned int lba, unsigned int offset, unsigned int count, unsigned char* out) {
    if (!out) return 0;
    if (!ensure_open()) return 0;

    size_t bytes = (size_t)count * SECTOR_SIZE;
    off_t off = (off_t)((uint64_t)lba * (uint64_t)SECTOR_SIZE + (uint64_t)offset);

    return (full_pread(out, bytes, off) == 0) ? 1 : 0;
}


unsigned char* DSK_read_sector(unsigned int lba) {
    return DSK_read_sectors(lba, 1);
}

unsigned char* DSK_read_sectors(unsigned int lba, unsigned int count) {
    size_t bytes = (size_t)count * SECTOR_SIZE;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return NULL;
    if (!DSK_read_sectors_into(lba, count, buf)) {
        free(buf);
        return NULL;
    }

    return buf;
}

unsigned char* DSK_readoff_sectors(unsigned int lba, unsigned int offset, unsigned int count) {
    size_t bytes = (size_t)count * SECTOR_SIZE;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return NULL;

    if (!DSK_readoff_sectors_into(lba, offset, count, buf)) {
        free(buf);
        return NULL;
    }
    return buf;
}

unsigned char* DSK_readoff_sectors_stop(unsigned int lba, unsigned int offset, unsigned int count, unsigned char* stop) {
    if (stop) *stop = 0;
    return DSK_readoff_sectors(lba, offset, count);
}

int DSK_write_sectors(unsigned int lba, const unsigned char* data, unsigned int count) {
    if (!data) return 0;
    if (!ensure_open()) return 0;

    size_t bytes = (size_t)count * SECTOR_SIZE;
    off_t off = (off_t)((uint64_t)lba * (uint64_t)SECTOR_SIZE);

    return (full_pwrite(data, bytes, off) == 0) ? 1 : 0;
}

int DSK_writeoff_sectors(unsigned int lba, const unsigned char* data, unsigned int count, unsigned int offset, unsigned int size) {
    if (!data) return 0;
    if (!ensure_open()) return 0;

    uint64_t window = (uint64_t)count * (uint64_t)SECTOR_SIZE;
    if ((uint64_t)offset > window) return 0;
    if ((uint64_t)size > window - (uint64_t)offset) return 0;

    off_t off = (off_t)((uint64_t)lba * (uint64_t)SECTOR_SIZE + (uint64_t)offset);

    return (full_pwrite(data, (size_t)size, off) == 0) ? 1 : 0;
}

int DSK_copy_sectors2sectors(unsigned int src_lba, unsigned int dst_lba, unsigned int count) {
    size_t bytes = (size_t)count * SECTOR_SIZE;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return 0;

    int ok = DSK_read_sectors_into(src_lba, count, buf) && DSK_write_sectors(dst_lba, buf, count);
    free(buf);
    return ok ? 1 : 0;
}
