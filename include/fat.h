#ifndef FAT_H_
#define FAT_H_

#include "disk.h"
#include "fslib.h"
#include "compat.h"
#include "dtime.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

#define END_CLUSTER_32      	0x0FFFFFF8
#define BAD_CLUSTER_32      	0x0FFFFFF7
#define FREE_CLUSTER_32     	0x00000000
#define END_CLUSTER_16      	0xFFF8
#define BAD_CLUSTER_16      	0xFFF7
#define FREE_CLUSTER_16     	0x0000
#define END_CLUSTER_12      	0xFF8
#define BAD_CLUSTER_12      	0xFF7
#define FREE_CLUSTER_12     	0x000

#define CLEAN_EXIT_BMASK_16 	0x8000
#define HARD_ERR_BMASK_16   	0x4000
#define CLEAN_EXIT_BMASK_32 	0x08000000
#define HARD_ERR_BMASK_32   	0x04000000

#define FILE_LONG_NAME 			(FILE_READ_ONLY|FILE_HIDDEN|FILE_SYSTEM|FILE_VOLUME_ID)
#define FILE_LONG_NAME_MASK 	(FILE_READ_ONLY|FILE_HIDDEN|FILE_SYSTEM|FILE_VOLUME_ID|FILE_DIRECTORY|FILE_ARCHIVE)

#define FILE_LAST_LONG_ENTRY    0x40
#define ENTRY_FREE              0xE5
#define ENTRY_END               0x00
#define ENTRY_JAPAN             0x05
#define LAST_LONG_ENTRY         0x40

#define LOWERCASE_ISSUE	        0x01
#define BAD_CHARACTER	        0x02
#define BAD_TERMINATION         0x04
#define NOT_CONVERTED_YET       0x08
#define TOO_MANY_DOTS           0x10

#define FILE_READ_ONLY 			0x01
#define FILE_HIDDEN    			0x02
#define FILE_SYSTEM    			0x04
#define FILE_VOLUME_ID 			0x08
#define FILE_DIRECTORY 			0x10
#define FILE_ARCHIVE   			0x20

#define FILE_LAST_LONG_ENTRY 	0x40
#define ENTRY_FREE           	0xE5
#define ENTRY_END            	0x00
#define ENTRY_JAPAN          	0x05
#define LAST_LONG_ENTRY      	0x40

#define LOWERCASE_ISSUE	  		0x01
#define BAD_CHARACTER	  		0x02
#define BAD_TERMINATION   		0x04
#define NOT_CONVERTED_YET 		0x08
#define TOO_MANY_DOTS     		0x10

#define GET_CLUSTER_FROM_ENTRY(x, fat_type)       (x.low_bits | (x.high_bits << (fat_type / 2)))
#define GET_CLUSTER_FROM_PENTRY(x, fat_type)      (x->low_bits | (x->high_bits << (fat_type / 2)))

#define GET_ENTRY_LOW_BITS(x, fat_type)           ((x) & ((1 << (fat_type / 2)) - 1))
#define GET_ENTRY_HIGH_BITS(x, fat_type)          ((x) >> (fat_type / 2))
#define CONCAT_ENTRY_HL_BITS(high, low, fat_type) ((high << (fat_type / 2)) | low)

#define CONTENT_TABLE_SIZE	50
#define PATH_DELIMITER      '/'

/* Bpb taken from http://wiki.osdev.org/FAT */

//FAT directory and bootsector structures
typedef struct fat_extBS_32 {
	unsigned int table_size_32;
	unsigned short extended_flags;
	unsigned short fat_version;
	unsigned int root_cluster;
	unsigned short fat_info;
	unsigned short backup_BS_sector;
	unsigned char  reserved_0[12];
	unsigned char	 drive_number;
	unsigned char  reserved_1;
	unsigned char	 boot_signature;
	unsigned int volume_id;
	unsigned char	 volume_label[11];
	unsigned char	 fat_type_label[8];
} __attribute__((packed)) fat_extBS_32_t;

typedef struct fat_BS {
	unsigned char  bootjmp[3];
	unsigned char  oem_name[8];
	unsigned short bytes_per_sector;
	unsigned char	 sectors_per_cluster;
	unsigned short reserved_sector_count;
	unsigned char	 table_count;
	unsigned short root_entry_count;
	unsigned short total_sectors_16;
	unsigned char	 media_type;
	unsigned short table_size_16;
	unsigned short sectors_per_track;
	unsigned short head_side_count;
	unsigned int hidden_sector_count;
	unsigned int total_sectors_32;
	unsigned char	 extended_section[54];
} __attribute__((packed)) fat_BS_t;

/* from http://wiki.osdev.org/FAT */
/* From file_system.h (CordellOS brunch FS_based_on_FAL) */

typedef struct fat_data {
	unsigned int fat_size;
	unsigned int fat_type;
	unsigned int first_data_sector;
	unsigned int total_sectors;
	unsigned int total_clusters;
	unsigned int bytes_per_sector;
	unsigned int cluster_size;
	unsigned int sectors_per_cluster;
	unsigned int ext_root_cluster;
	unsigned int first_fat_sector;
	unsigned int table_count;
} fat_data_t;

typedef struct directory_entry {
	unsigned char file_name[11];
	unsigned char attributes;
	unsigned char reserved0;
	unsigned char creation_time_tenths;
	unsigned short creation_time;
	unsigned short creation_date;
	unsigned short last_accessed;
	unsigned short high_bits;
	unsigned short last_modification_time;
	unsigned short last_modification_date;
	unsigned short low_bits;
	unsigned int file_size;
} __attribute__((packed)) directory_entry_t;

typedef struct FATFile {
	char name[8];
	char extension[4];
	int data_size;
	unsigned int* data;
    struct FATFile* next;
} File;

typedef struct FATDirectory {
	char name[11];
	struct FATDirectory* next;
    struct FATFile* files;
    struct FATDirectory* subDirectory;
} Directory;

typedef enum {
	CONTENT_TYPE_FILE,
	CONTENT_TYPE_DIRECTORY
} ContentType;

typedef struct FATContent {
	union {
		Directory* directory;
		File* file;
	};
	
	unsigned int parent_cluster;
	directory_entry_t meta;
	ContentType content_type;
} Content;

extern fat_data_t FAT_data;

int FAT_initialize(); 
int FAT_directory_list(int ci, unsigned char attrs, int exclusive);

int FAT_content_exists(const char* path);
int FAT_open_content(const char* path);
int FAT_close_content(int ci);
int FAT_read_content2buffer(int ci, unsigned char* buffer, unsigned int offset, unsigned int size);
//int FAT_read_content2buffer_stop(int ci, unsigned char* buffer, unsigned int offset, unsigned int size, unsigned char* stop);
int FAT_put_content(const char* path, Content* content);
int FAT_delete_content(const char* path);
int FAT_write_buffer2content(int ci, const unsigned char* buffer, unsigned int offset, unsigned int size);
//int FAT_ELF_execute_content(int ci, int argc, char* argv[], int type);
int FAT_change_meta(const char* path, const char* new_name);
int FAT_stat_content(int ci, CInfo_t* info);

unsigned short _current_date();
void _fatname2name(char* input, char* output);
char* _name2fatname(char* input);
int _name_check(const char* input);

int _add_content2table(Content* content);
Content* FAT_get_content_from_table(int ci) ;
int _remove_content_from_table(int index);

Content* FAT_create_object(char* name, int is_directory, char* extension);
Content* FAT_create_content();
int FAT_unload_content_system(Content* content);
Directory* _create_directory();
File* _create_file();

int fat_cache_init();
void fat_cache_free_all();

#endif