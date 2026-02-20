#ifndef DISK_H_
#define DISK_H_

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#define SECTOR_SIZE 512

int  DSK_host_open(const char* image_path);
void DSK_host_close(void);

unsigned char* DSK_read_sector(unsigned int lba);
unsigned char* DSK_read_sectors(unsigned int lba, unsigned int sector_count);
unsigned char* DSK_readoff_sectors(unsigned int lba, unsigned int offset, unsigned int sector_count);
unsigned char* DSK_readoff_sectors_stop(unsigned int lba, unsigned int offset, unsigned int sector_count, unsigned char* stop);

int DSK_write_sectors(unsigned int lba, const unsigned char* data, unsigned int sector_count);
int DSK_writeoff_sectors(unsigned int lba, const unsigned char* data, unsigned int sector_count, unsigned int offset, unsigned int size);

int DSK_copy_sectors2sectors(unsigned int src_lba, unsigned int dst_lba, unsigned int sector_count);

int DSK_read_sectors_into(unsigned int lba, unsigned int sector_count, unsigned char* out);
int DSK_readoff_sectors_into(unsigned int lba, unsigned int offset, unsigned int sector_count, unsigned char* out);

#endif
