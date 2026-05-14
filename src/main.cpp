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

    SdSpiConfig config(PIN_CS, DEDICATED_SPI, 250000, &SPI);

    card = cardFactory.newCard(config);
    if (card == NULL) {
        Serial.println("card init failed");
        return;
    }

    Serial.println("card init ok");

    uint32_t sectors = card->sectorCount();

    Serial.print("card sectors: ");
    Serial.println(sectors);

    raw_smoke_test();

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

    Serial.println("test done");
}

void loop() {
}