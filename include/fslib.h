#ifndef FSLIB_H_
#define FSLIB_H_

#include <stdint.h>
#include <stddef.h>

#define STAT_FILE   0x00
#define STAT_DIR    0x01
#define NOT_PRESENT 0x02

typedef struct FATDate {
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t year;
    uint16_t mounth;
    uint16_t day;
} Date;

typedef struct {
    unsigned char full_name[11];
    char file_name[8];
    char file_extension[4];
    int type;
    int size;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_accessed;
    uint16_t last_modification_time;
    uint16_t last_modification_date;
} CInfo_t;

typedef struct {
    char mount[32];
    char name[24];
    unsigned int type;
    unsigned int clusters;
    uint16_t spc;
    unsigned int size;
} FSInfo_t;

int  FSLIB_get_date_into(uint16_t data, int type, Date* out);
int  FSLIB_change_path_into(const char* currentPath, const char* content, char* out, size_t outsz);

char* FSLIB_change_path(const char* currentPath, const char* content);
Date* FSLIB_get_date(uint16_t data, int type);

#endif
