#ifndef FSLIB_H_
#define FSLIB_H_

#include "stdio.h"
#include "stdlib.h"
#include "ctype.h"
#include "stdint.h"
#include <string.h>


#define STAT_FILE	0x00
#define STAT_DIR	0x01
#define NOT_PRESENT	0x02


typedef struct FATDate {
	unsigned short hour;
	unsigned short minute;
	unsigned short second;
	unsigned short year;
	unsigned short mounth;
	unsigned short day;
} Date;

typedef struct {
	unsigned char full_name[11];
	char file_name[8];
	char file_extension[4];
	int type;
	int size;
	unsigned short creation_time;
	unsigned short creation_date;
	unsigned short last_accessed;
	unsigned short last_modification_time;
	unsigned short last_modification_date;
} CInfo_t;

typedef struct {
	char mount[32];
	char name[24];
	unsigned int type;
	unsigned int clusters;
	unsigned short spc;
	unsigned int size;
} FSInfo_t;


char* FSLIB_change_path(const char* currentPath, const char* content);
Date* FSLIB_get_date(short data, int type);

#endif