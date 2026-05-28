#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <FatLib/FatFormatter.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "nifat32.h"
}

#define RUN_BASELINE 1
#define RUN_NIFAT32_TEST 1
#define RUN_FAT32_TEST 1

#define FORMAT_NIFAT32_BEFORE_TEST 1
#define FORMAT_FAT32_BEFORE_TEST 1

#define TEST_FILES_COUNT 200
#define SAMPLE_EVERY_FILES 10

// По прошлому замеру 200 файлов было около 173672 ms.
#define BASELINE_DURATION_MS 180000
#define BASELINE_SAMPLE_PERIOD_MS 10000

#define SPI_SPEED_HZ 250000

#define NIFAT32_TEST_SECTORS 65536

const int PIN_MISO = 19;
const int PIN_MOSI = 23;
const int PIN_CLK = 18;
const int PIN_CS = 13;
const int SECTOR_SIZE = 512;

SdCardFactory cardFactory;
SdCard *card = NULL;

SdFat fat32_sd;
Adafruit_INA219 ina219;
bool ina_ok = false;

RTC_NOINIT_ATTR uint32_t fs_compare_magic;
RTC_NOINIT_ATTR int fs_compare_phase;
const uint32_t FS_COMPARE_MAGIC = 0xA55AF132;







// ina219

void init_ina219() {
    Wire.begin(21, 22);

    if (!ina219.begin()) {
        Serial.println("INA219 init failed");
        ina_ok = false;
        return;
    }

    ina219.setCalibration_32V_2A();
    ina_ok = true;
    Serial.println("INA219 init ok");
}

void print_power_csv(const char *label, int files_count, int sample_point, uint32_t elapsed_ms) {
    if (!ina_ok) {
        return;
    }

    float bus_v = ina219.getBusVoltage_V();
    float shunt_mv = ina219.getShuntVoltage_mV();
    float current_ma = ina219.getCurrent_mA();
    float power_mw = ina219.getPower_mW();

    Serial.print("CSV_POWER,");
    Serial.print(label);
    Serial.print(",");
    Serial.print(files_count);
    Serial.print(",");
    Serial.print(sample_point);
    Serial.print(",");
    Serial.print(elapsed_ms);
    Serial.print(",");
    Serial.print(bus_v, 3);
    Serial.print(",");
    Serial.print(shunt_mv, 3);
    Serial.print(",");
    Serial.print(current_ma, 3);
    Serial.print(",");
    Serial.println(power_mw, 3);
}

void print_csv_header() {
    Serial.println("CSV_POWER_HEADER,label,files_count,sample_point,elapsed_ms,bus_V,shunt_mV,current_mA,power_mW");
    Serial.println("CSV_RESULT_HEADER,label,files_count,ok_count,total_ms,avg_open_us,avg_write_512B_us,avg_close_us,files_per_sec");
}

// raw disk adapter

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

int write_raw_sector_with_retry(uint32_t sector, const unsigned char *data) {
    for (int attempt = 0; attempt < 5; attempt++) {
        if (card->writeSector(sector, data)) {
            return 1;
        }

        Serial.print("write retry sector=");
        Serial.print(sector);
        Serial.print(" attempt=");
        Serial.println(attempt + 1);

        delay(20);
    }

    Serial.print("write failed sector=");
    Serial.println(sector);
    return 0;
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
        return write_raw_sector_with_retry(sa, tmp);
    }

    return write_raw_sector_with_retry(sa, data);
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










// NIFAT32 formatter

#define NFT_SECTORS_PER_CLUSTER 8
#define NFT_RESERVED_SECTORS 32
#define NFT_FAT_COUNT 4
#define NFT_ROOT_DIR_CLUSTER 2
#define NFT_FAT_ENTRY_FREE 0x00000000
#define NFT_FAT_ENTRY_END 0x0FFFFFFF
#define NFT_FAT_ENTRY_RESERVED 0x0FFFFFF8

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
    return write_raw_sector_with_retry(sector, data);
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

    Serial.print("format NiFAT32 total sectors: ");
    Serial.println(total_sectors);

    Serial.print("format NiFAT32 fat size: ");
    Serial.println(fat_size);

    Serial.print("format NiFAT32 first data sector: ");
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

    Serial.println("format NiFAT32 done");
    return 1;
}










// ======================= baseline =======================

void baseline_empty_loop_benchmark(const char *label, int files_count, uint32_t duration_ms, uint32_t sample_period_ms) {
    Serial.print("baseline start: ");
    Serial.println(label);

    uint32_t start_ms = millis();
    uint32_t last_sample_ms = start_ms;

    print_power_csv(label, files_count, 0, 0);

    while (millis() - start_ms < duration_ms) {
        uint32_t now = millis();
        uint32_t elapsed = now - start_ms;

        if (now - last_sample_ms >= sample_period_ms) {
            print_power_csv(label, files_count, elapsed, elapsed);
            last_sample_ms = now;
        }

        delay(1);
    }

    print_power_csv(label, files_count, duration_ms, duration_ms);

    Serial.print("baseline total ms: ");
    Serial.println(duration_ms);
    Serial.println("baseline done");
}

// NIFAT32 init, benchmark

int init_nifat32(uint32_t sectors) {
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

    return ok;
}

void make_nifat32_name(char out[12], int i) {
    memset(out, ' ', 11);
    out[11] = 0;

    char num[8];
    snprintf(num, sizeof(num), "%07d", i);

    out[0] = 'N';
    memcpy(out + 1, num, 7);
    memcpy(out + 8, "DAT", 3);
}

void fill_mock_data(unsigned char *buf, int file_index, int bench_size) {
    for (int i = 0; i < SECTOR_SIZE; i++) {
        buf[i] = (unsigned char)((i * 31 + file_index * 17 + bench_size) & 0xFF);
    }
}

void nifat32_many_files_benchmark_one(const char *label, int files_count) {
    Serial.print("nifat32 many files benchmark start: ");
    Serial.println(files_count);

    unsigned char write_buf[SECTOR_SIZE];

    uint64_t open_us = 0;
    uint64_t write_us = 0;
    uint64_t close_us = 0;
    int ok_count = 0;

    uint32_t total_start_ms = millis();
    print_power_csv(label, files_count, 0, 0);

    for (int i = 0; i < files_count; i++) {
        char name[12];
        make_nifat32_name(name, i);
        fill_mock_data(write_buf, i, files_count);

        uint32_t t0 = micros();
        ci_t file = NIFAT32_open_content(NO_RCI, name, MODE(CR_MODE | R_MODE | W_MODE, FILE_TARGET));
        open_us += micros() - t0;

        if (file < 0) {
            Serial.print("nifat32 open failed at ");
            Serial.println(i);
            continue;
        }

        t0 = micros();
        int written = NIFAT32_write_buffer2content(file, 0, write_buf, SECTOR_SIZE);
        write_us += micros() - t0;

        t0 = micros();
        NIFAT32_close_content(file);
        close_us += micros() - t0;

        if (written == SECTOR_SIZE) {
            ok_count++;
        }

        if ((i + 1) % SAMPLE_EVERY_FILES == 0) {
            Serial.print("nifat32 progress ");
            Serial.print(label);
            Serial.print(" files=");
            Serial.println(i + 1);

            print_power_csv(label, files_count, i + 1, millis() - total_start_ms);
        }
    }

    uint32_t total_ms = millis() - total_start_ms;

    uint32_t avg_open = files_count > 0 ? (uint32_t)(open_us / files_count) : 0;
    uint32_t avg_write = files_count > 0 ? (uint32_t)(write_us / files_count) : 0;
    uint32_t avg_close = files_count > 0 ? (uint32_t)(close_us / files_count) : 0;
    float fps = total_ms > 0 ? (float)files_count * 1000.0 / (float)total_ms : 0.0;

    Serial.print("nifat32 many files result: ");
    Serial.print(ok_count);
    Serial.print("/");
    Serial.println(files_count);
    Serial.print("nifat32 total ms: ");
    Serial.println(total_ms);
    Serial.print("nifat32 avg create/open us: ");
    Serial.println(avg_open);
    Serial.print("nifat32 avg write 512B us: ");
    Serial.println(avg_write);
    Serial.print("nifat32 avg close us: ");
    Serial.println(avg_close);
    Serial.print("nifat32 files per sec: ");
    Serial.println(fps, 3);

    print_power_csv(label, files_count, files_count, total_ms);

    Serial.print("CSV_RESULT,");
    Serial.print(label);
    Serial.print(",");
    Serial.print(files_count);
    Serial.print(",");
    Serial.print(ok_count);
    Serial.print(",");
    Serial.print(total_ms);
    Serial.print(",");
    Serial.print(avg_open);
    Serial.print(",");
    Serial.print(avg_write);
    Serial.print(",");
    Serial.print(avg_close);
    Serial.print(",");
    Serial.println(fps, 3);

    Serial.println("nifat32 many files benchmark done");
}

// FAT 32

int format_fat32_on_card() {
    Serial.println("format FAT32 start");

    FatFormatter fat_formatter;
    static uint8_t sector_buffer[SECTOR_SIZE];

    memset(sector_buffer, 0, sizeof(sector_buffer));

    bool ok = fat_formatter.format(card, sector_buffer, &Serial);

    Serial.print("format FAT32: ");
    Serial.println(ok ? "ok" : "failed");

    return ok ? 1 : 0;
}

void make_fat32_name(char *out, int i) {
    snprintf(out, 16, "F%07d.TXT", i);
}

void fat32_many_files_benchmark_one(const char *label, int files_count) {
    Serial.print("fat32 many files benchmark start: ");
    Serial.println(files_count);

    SdSpiConfig config(PIN_CS, DEDICATED_SPI, SPI_SPEED_HZ, &SPI);

    if (!fat32_sd.begin(config)) {
        Serial.println("FAT32 begin failed");
        Serial.println("Check that format FAT32 succeeded");
        return;
    }

    Serial.println("FAT32 begin ok");

    unsigned char write_buf[SECTOR_SIZE];

    for (int i = 0; i < SECTOR_SIZE; i++) {
        write_buf[i] = (i * 11) % 256;
    }

    uint64_t open_us = 0;
    uint64_t write_us = 0;
    uint64_t close_us = 0;
    int ok_count = 0;

    uint32_t total_start_ms = millis();
    print_power_csv(label, files_count, 0, 0);

    for (int i = 0; i < files_count; i++) {
        char name[16];
        make_fat32_name(name, i);

        fat32_sd.remove(name);

        uint32_t t0 = micros();
        File file = fat32_sd.open(name, O_RDWR | O_CREAT | O_TRUNC);
        open_us += micros() - t0;

        if (!file) {
            Serial.print("fat32 open failed at ");
            Serial.println(i);
            continue;
        }

        t0 = micros();
        size_t written = file.write(write_buf, SECTOR_SIZE);
        write_us += micros() - t0;

        t0 = micros();
        file.close();
        close_us += micros() - t0;

        if (written == SECTOR_SIZE) {
            ok_count++;
        }

        if ((i + 1) % SAMPLE_EVERY_FILES == 0) {
            Serial.print("fat32 progress ");
            Serial.print(label);
            Serial.print(" files=");
            Serial.println(i + 1);

            print_power_csv(label, files_count, i + 1, millis() - total_start_ms);
        }
    }

    uint32_t total_ms = millis() - total_start_ms;

    uint32_t avg_open = files_count > 0 ? (uint32_t)(open_us / files_count) : 0;
    uint32_t avg_write = files_count > 0 ? (uint32_t)(write_us / files_count) : 0;
    uint32_t avg_close = files_count > 0 ? (uint32_t)(close_us / files_count) : 0;
    float fps = total_ms > 0 ? (float)files_count * 1000.0 / (float)total_ms : 0.0;

    Serial.print("fat32 many files result: ");
    Serial.print(ok_count);
    Serial.print("/");
    Serial.println(files_count);
    Serial.print("fat32 total ms: ");
    Serial.println(total_ms);
    Serial.print("fat32 avg create/open us: ");
    Serial.println(avg_open);
    Serial.print("fat32 avg write 512B us: ");
    Serial.println(avg_write);
    Serial.print("fat32 avg close us: ");
    Serial.println(avg_close);
    Serial.print("fat32 files per sec: ");
    Serial.println(fps, 3);

    print_power_csv(label, files_count, files_count, total_ms);

    Serial.print("CSV_RESULT,");
    Serial.print(label);
    Serial.print(",");
    Serial.print(files_count);
    Serial.print(",");
    Serial.print(ok_count);
    Serial.print(",");
    Serial.print(total_ms);
    Serial.print(",");
    Serial.print(avg_open);
    Serial.print(",");
    Serial.print(avg_write);
    Serial.print(",");
    Serial.print(avg_close);
    Serial.print(",");
    Serial.println(fps, 3);

    Serial.println("fat32 many files benchmark done");
}


void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("ESP32 filesystem compare test start");

    init_ina219();
    print_csv_header();

    SPI.begin(PIN_CLK, PIN_MISO, PIN_MOSI, PIN_CS);

    if (fs_compare_magic == FS_COMPARE_MAGIC && fs_compare_phase == 1) {
        Serial.println("phase 1: FAT32 benchmark after software restart");
        fs_compare_phase = 0;
        fs_compare_magic = 0;

#if RUN_FAT32_TEST
        fat32_many_files_benchmark_one("FAT32_A2", TEST_FILES_COUNT);
#else
        Serial.println("RUN_FAT32_TEST is disabled");
#endif

        Serial.println("test done");
        return;
    }

    Serial.println("phase 0: baseline + NiFAT32 + FAT32 format");

    SdSpiConfig config(PIN_CS, DEDICATED_SPI, SPI_SPEED_HZ, &SPI);

    card = cardFactory.newCard(config);
    if (card == NULL) {
        Serial.println("card init failed");
        return;
    }

    Serial.println("card init ok");

    uint32_t sectors = card->sectorCount();

    Serial.print("card sectors: ");
    Serial.println(sectors);

    if (sectors == 0) {
        Serial.print("error code: 0x");
        Serial.println(card->errorCode(), HEX);
        Serial.print("error data: 0x");
        Serial.println(card->errorData(), HEX);
        return;
    }

    uint32_t nifat32_sectors = sectors;
    if (nifat32_sectors > NIFAT32_TEST_SECTORS) {
        nifat32_sectors = NIFAT32_TEST_SECTORS;
    }

    Serial.print("NiFAT32 test sectors: ");
    Serial.println(nifat32_sectors);

#if RUN_BASELINE
    baseline_empty_loop_benchmark("BASE_EMPTY", TEST_FILES_COUNT, BASELINE_DURATION_MS, BASELINE_SAMPLE_PERIOD_MS);
#endif

#if RUN_NIFAT32_TEST
#if FORMAT_NIFAT32_BEFORE_TEST
    if (!format_nifat32_on_card(nifat32_sectors)) {
        Serial.println("NiFAT32 format failed");
        return;
    }
#endif

    if (!init_nifat32(nifat32_sectors)) {
        Serial.println("NiFAT32 init failed");
        return;
    }

    nifat32_many_files_benchmark_one("NIFAT32_A2", TEST_FILES_COUNT);
#endif

#if RUN_FAT32_TEST
#if FORMAT_FAT32_BEFORE_TEST
    if (!format_fat32_on_card()) {
        Serial.println("FAT32 format failed");
        return;
    }
#endif

    Serial.println("FAT32 formatted, restarting before FAT32 benchmark");
    fs_compare_magic = FS_COMPARE_MAGIC;
    fs_compare_phase = 1;
    delay(1000);
    ESP.restart();
#else
    Serial.println("test done");
#endif
}

void loop() {
}
