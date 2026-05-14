#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


extern "C" {
#include "nifat32.h"
}

const int PIN_MISO = 19;
const int PIN_MOSI = 23;
const int PIN_CLK = 18;
const int PIN_CS = 13;

const int SECTOR_SIZE = 512;

SdCardFactory cardFactory;
SdCard *card = NULL;

#define NFT_SECTORS_PER_CLUSTER 8
#define NFT_RESERVED_SECTORS 32
#define NFT_FAT_COUNT 4
#define NFT_ROOT_DIR_CLUSTER 2
#define NFT_FAT_ENTRY_FREE 0x00000000
#define NFT_FAT_ENTRY_END 0x0FFFFFFF
#define NFT_FAT_ENTRY_RESERVED 0x0FFFFFF8

#define BOOT_MULTIPLIER 2654435761U
#define FAT_MULTIPLIER 340573321U
#define GET_BOOTSECTOR(n, ts) (((((n) + 1) * BOOT_MULTIPLIER) >> 11) % ((ts) - 2))
#define GET_FATSECTOR(n, ts) (((((n) + 7) * FAT_MULTIPLIER) >> 13) % ((ts) - 32))

uint32_t calculate_fat_size(uint32_t total_sectors) {
    uint32_t data_sectors = total_sectors - NFT_RESERVED_SECTORS;
    uint32_t fat_size = 0;
    uint32_t prev_fat_size = 0;

    do {
        prev_fat_size = fat_size;
        uint32_t clusters = (data_sectors - NFT_FAT_COUNT * fat_size) / NFT_SECTORS_PER_CLUSTER;
        fat_size = (clusters * sizeof(uint32_t) + SECTOR_SIZE - 1) / SECTOR_SIZE;
    } while (fat_size != prev_fat_size);

    return fat_size;
}

int write_full_sector(uint32_t sector, const unsigned char *data) {
    return card->writeSector(sector, data) ? 1 : 0;
}

int write_zero_sectors(uint32_t sector, uint32_t count) {
    unsigned char zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);

    for (uint32_t i = 0; i < count; i++) {
        if (!write_full_sector(sector + i, zero)) {
            return 0;
        }
    }

    return 1;
}

int write_boot_sectors(uint32_t total_sectors, uint32_t fat_size, uint8_t bs_count) {
    nifat32_bootsector_t bs = {};

    bs.bootjmp[0] = 0xEB;
    bs.bootjmp[1] = 0x58;
    bs.bootjmp[2] = 0x90;
    memcpy(bs.oem_name, "NIFAT 32", 8);
    bs.bytes_per_sector = SECTOR_SIZE;
    bs.sectors_per_cluster = NFT_SECTORS_PER_CLUSTER;
    bs.reserved_sector_count = NFT_RESERVED_SECTORS;
    bs.table_count = NFT_FAT_COUNT;
    bs.root_entry_count = 0;
    bs.total_sectors_16 = 0;
    bs.media_type = 0xF8;
    bs.table_size_16 = 0;
    bs.sectors_per_track = 63;
    bs.head_side_count = 255;
    bs.hidden_sector_count = 0;
    bs.total_sectors_32 = total_sectors;

    nifat32_ext32_bootsector_t ext = {};
    ext.table_size_32 = fat_size;
    ext.root_cluster = NFT_ROOT_DIR_CLUSTER;
    ext.drive_number = 0x80;
    ext.boot_signature = 0x29;
    ext.volume_id = 0x12345678;
    memcpy(ext.volume_label, "ROOT_LABEL ", 11);
    memcpy(ext.fat_type_label, "NIFAT32 ", 8);
    ext.checksum = 0;
    ext.checksum = nft32_murmur3_x86_32((const unsigned char *)&ext, sizeof(ext), 0);

    memcpy(&bs.extended_section, &ext, sizeof(ext));

    bs.checksum = 0;
    bs.checksum = nft32_murmur3_x86_32((const unsigned char *)&bs, sizeof(bs), 0);

    unsigned char sector[SECTOR_SIZE];
    unsigned short encoded[sizeof(nifat32_bootsector_t)];

    for (int i = 0; i < bs_count; i++) {
        memset(sector, 0, SECTOR_SIZE);
        memset(encoded, 0, sizeof(encoded));

        nft32_pack_memory((const unsigned char *)&bs, encoded, sizeof(nifat32_bootsector_t));
        memcpy(sector, encoded, sizeof(encoded));

        uint32_t sa = GET_BOOTSECTOR(i, total_sectors);

        Serial.print("write boot sector ");
        Serial.println(sa);

        if (!write_full_sector(sa, sector)) {
            return 0;
        }
    }

    return 1;
}

void nifat32_file_test() {
    Serial.println("NiFAT32 file test start");

    ci_t file = NIFAT32_open_content(NO_RCI, "TEST    TXT", MODE(CR_MODE | R_MODE | W_MODE, FILE_TARGET));
    if (file < 0) {
        Serial.print("open/create file failed: ");
        Serial.println(file);
        return;
    }

    Serial.println("open/create file ok");

    const char *msg = "Hello from NiFAT32 on ESP32";
    int written = NIFAT32_write_buffer2content(file, 0, (const unsigned char *)msg, strlen(msg));

    Serial.print("written: ");
    Serial.println(written);

    char buf[64];
    memset(buf, 0, sizeof(buf));

    int read_n = NIFAT32_read_content2buffer(file, 0, (unsigned char *)buf, strlen(msg));

    Serial.print("read: ");
    Serial.println(read_n);

    Serial.print("content: ");
    Serial.println(buf);

    Serial.print("compare: ");
    Serial.println(strcmp(buf, msg) == 0 ? "ok" : "failed");

    NIFAT32_close_content(file);

    Serial.println("NiFAT32 file test done");
}

int write_fats(uint32_t total_sectors, uint32_t fat_size) {
    uint32_t total_clusters = (total_sectors - NFT_RESERVED_SECTORS - NFT_FAT_COUNT * fat_size) / NFT_SECTORS_PER_CLUSTER;
    uint32_t fat_entries_per_sector = SECTOR_SIZE / sizeof(uint32_t);

    unsigned char raw[SECTOR_SIZE];
    unsigned char encoded_sector[SECTOR_SIZE];
    unsigned short encoded_half[SECTOR_SIZE / 2];

    for (int fat_id = 0; fat_id < NFT_FAT_COUNT; fat_id++) {
        uint32_t fat_start = NFT_RESERVED_SECTORS + GET_FATSECTOR(fat_id, total_sectors);

        Serial.print("write FAT ");
        Serial.print(fat_id);
        Serial.print(" at ");
        Serial.println(fat_start);

        for (uint32_t s = 0; s < fat_size; s++) {
            memset(raw, 0, SECTOR_SIZE);
            memset(encoded_sector, 0, SECTOR_SIZE);

            uint32_t *entries = (uint32_t *)raw;

            for (uint32_t j = 0; j < fat_entries_per_sector; j++) {
                uint32_t cluster = s * fat_entries_per_sector + j;
                uint32_t value = NFT_FAT_ENTRY_FREE;

                if (cluster == 0) {
                    value = NFT_FAT_ENTRY_RESERVED | (0xF8 << 24);
                } else if (cluster == 1 || cluster == 2) {
                    value = NFT_FAT_ENTRY_END | (0xF8 << 24);
                }

                if (cluster < total_clusters) {
                    entries[j] = value;
                }
            }

            nft32_pack_memory(raw, encoded_half, SECTOR_SIZE / 2);
            memcpy(encoded_sector, encoded_half, SECTOR_SIZE);

            if (!write_full_sector(fat_start + s, encoded_sector)) {
                return 0;
            }
        }
    }

    return 1;
}

int format_nifat32_on_card(uint32_t total_sectors) {
    uint32_t fat_size = calculate_fat_size(total_sectors);
    uint32_t first_data_sector = NFT_RESERVED_SECTORS + NFT_FAT_COUNT * fat_size;

    Serial.print("format total sectors: ");
    Serial.println(total_sectors);

    Serial.print("format fat size: ");
    Serial.println(fat_size);

    Serial.print("format first data sector: ");
    Serial.println(first_data_sector);

    if (!write_boot_sectors(total_sectors, fat_size, 5)) {
        Serial.println("write boot sectors failed");
        return 0;
    }

    if (!write_fats(total_sectors, fat_size)) {
        Serial.println("write fats failed");
        return 0;
    }

    if (!write_zero_sectors(first_data_sector, NFT_SECTORS_PER_CLUSTER)) {
        Serial.println("write root dir failed");
        return 0;
    }

    Serial.println("format done");
    return 1;
}


int esp32_read_sector(sector_addr_t sa, sector_offset_t offset, unsigned char *buffer, int size) {
    if (offset < 0 || offset + size > SECTOR_SIZE) {
        return 0;
    }

    unsigned char tmp[SECTOR_SIZE];

    if (!card->readSector(sa, tmp)) {
        return 0;
    }

    memcpy(buffer, tmp + offset, size);
    return 1;
}

int esp32_write_sector(sector_addr_t sa, sector_offset_t offset, const unsigned char *data, int size) {
    if (offset < 0 || offset + size > SECTOR_SIZE) {
        return 0;
    }

    unsigned char tmp[SECTOR_SIZE];

    if (offset != 0 || size != SECTOR_SIZE) {
        if (!card->readSector(sa, tmp)) {
            return 0;
        }
        memcpy(tmp + offset, data, size);
        return card->writeSector(sa, tmp) ? 1 : 0;
    }

    return card->writeSector(sa, data) ? 1 : 0;
}

int serial_fprintf(const char *fmt, ...) {
    char buf[256];

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.print(buf);
    return n;
}

int serial_vfprintf(const char *fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    Serial.print(buf);
    return n;
}

void *esp32_malloc(unsigned long size) {
    return malloc(size);
}

void esp32_free(void *ptr) {
    free(ptr);
}

void raw_smoke_test() {
    Serial.println("raw smoke test");

    unsigned char write_buf[SECTOR_SIZE];
    unsigned char read_buf[SECTOR_SIZE];

    for (int i = 0; i < SECTOR_SIZE; i++) {
        write_buf[i] = i % 256;
    }

    const uint32_t test_sector = 2048;

    if (!card->writeSector(test_sector, write_buf)) {
        Serial.println("raw write failed");
        return;
    }

    if (!card->readSector(test_sector, read_buf)) {
        Serial.println("raw read failed");
        return;
    }

    bool same = true;
    for (int i = 0; i < SECTOR_SIZE; i++) {
        if (write_buf[i] != read_buf[i]) {
            same = false;
            break;
        }
    }

    Serial.print("raw compare: ");
    Serial.println(same ? "ok" : "failed");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("ESP32 NiFAT32 test start");

    SPI.begin(PIN_CLK, PIN_MISO, PIN_MOSI, PIN_CS);

    SdSpiConfig config(PIN_CS, DEDICATED_SPI, 500000, &SPI);

    card = cardFactory.newCard(config);
    if (card == NULL) {
        Serial.println("card init failed");
        return;
    }

    Serial.println("card init ok");

    uint32_t sectors = card->sectorCount();

    Serial.print("card sectors: ");
    Serial.println(sectors);

    //raw_smoke_test();

    //if (!format_nifat32_on_card(sectors)) {
    //    Serial.println("format failed");
    //    return;
    //}

    nifat32_params_t params = {};

    params.bs_num = 0;
    params.bs_count = 5;
    params.ts = sectors;
    params.jc = 0;
    params.ec = 0;
    params.fat_cache = NO_CACHE;

    params.disk_io.read_sector = esp32_read_sector;
    params.disk_io.write_sector = esp32_write_sector;
    params.disk_io.sector_size = SECTOR_SIZE;

    params.logg_io.fd_fprintf = serial_fprintf;
    params.logg_io.fd_vfprintf = serial_vfprintf;

    params.mm_manager.init = NULL;
    params.mm_manager.malloc = esp32_malloc;
    params.mm_manager.free = esp32_free;
    Serial.println("before NIFAT32_init");

    int ok = NIFAT32_init(&params);

    Serial.print("NIFAT32_init: ");
    Serial.println(ok ? "ok" : "failed");

    if (!ok) {
    Serial.println("test done");
    return;
    }

    nifat32_file_test();

    Serial.println("test done");
}

void loop() {
}