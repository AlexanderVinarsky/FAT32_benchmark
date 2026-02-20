#include "fat.h"
#include <stdint.h>

fat_data_t FAT_data;

static Content* _content_table[CONTENT_TABLE_SIZE];
static unsigned int last_allocated_cluster = 2;

static uint16_t rd16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int looks_like_bpb(const unsigned char* s) {
    uint16_t bps = rd16(s + 11);
    unsigned char spc = s[13];
    uint16_t rsv = rd16(s + 14);
    unsigned char fats = s[16];

    int bps_ok = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096);
    int spc_ok = (spc == 1 || spc == 2 || spc == 4 || spc == 8 || spc == 16 || spc == 32 || spc == 64 || spc == 128);
    int rsv_ok = (rsv >= 1);
    int fats_ok = (fats >= 1 && fats <= 4);
    return bps_ok && spc_ok && rsv_ok && fats_ok;
}

int FAT_initialize() {
    unsigned int boot_lba = 0;
    unsigned char* s0 = DSK_read_sector(0);
    if (!s0) {
        printf("FAT_initialize: cannot read sector 0\n");
        return -1;
    }

    if (!looks_like_bpb(s0)) {
        if (s0[510] == 0x55 && s0[511] == 0xAA) {
            unsigned int p0 = 446;
            unsigned char ptype = s0[p0 + 4];
            unsigned int lba_start = rd32(s0 + p0 + 8);

            if ((ptype == 0x0B || ptype == 0x0C) && lba_start != 0) {
                unsigned char* bs = DSK_read_sector(lba_start);
                if (bs && looks_like_bpb(bs)) {
                    boot_lba = lba_start;
                }
                if (bs) free(bs);
            }
        }
    }
    free(s0);

    unsigned char* cluster_data = DSK_read_sector(boot_lba);
    if (!cluster_data) {
        printf("FAT_initialize: cannot read boot sector\n");
        return -1;
    }

    fat_BS_t* bpb = (fat_BS_t*)cluster_data;

    FAT_data.bytes_per_sector = bpb->bytes_per_sector;
    FAT_data.sectors_per_cluster = bpb->sectors_per_cluster;
    FAT_data.cluster_size = FAT_data.bytes_per_sector * FAT_data.sectors_per_cluster;
    FAT_data.table_count = bpb->table_count;

    FAT_data.total_sectors = (bpb->total_sectors_16 == 0) ? bpb->total_sectors_32 : bpb->total_sectors_16;

    unsigned int fat_size = (bpb->table_size_16 != 0)
        ? (unsigned int)bpb->table_size_16
        : ((fat_extBS_32_t*)(bpb->extended_section))->table_size_32;

    FAT_data.fat_size = fat_size;
    FAT_data.fat_type = 32;
    FAT_data.ext_root_cluster = ((fat_extBS_32_t*)(bpb->extended_section))->root_cluster;

    unsigned int root_dir_sectors =
        ((unsigned int)bpb->root_entry_count * 32u + (FAT_data.bytes_per_sector - 1u)) / FAT_data.bytes_per_sector;

    FAT_data.first_fat_sector  = boot_lba + (unsigned int)bpb->reserved_sector_count;
    FAT_data.first_data_sector = boot_lba + (unsigned int)bpb->reserved_sector_count
                               + (unsigned int)bpb->table_count * fat_size
                               + root_dir_sectors;

    unsigned int data_sectors = FAT_data.total_sectors
        - ((unsigned int)bpb->reserved_sector_count + (unsigned int)bpb->table_count * fat_size + root_dir_sectors);

    FAT_data.total_clusters = data_sectors / FAT_data.sectors_per_cluster;

    free(cluster_data);

    for (int i = 0; i < CONTENT_TABLE_SIZE; i++) _content_table[i] = NULL;

    last_allocated_cluster = 2;

    return 0;
}









static int __read_fat(unsigned int cluster) {
    unsigned int fat_offset = cluster * 4u;
    unsigned int fat_sector = FAT_data.first_fat_sector + (fat_offset / FAT_data.bytes_per_sector);
    unsigned int ent_off    = fat_offset % FAT_data.bytes_per_sector;

    unsigned int sec_count = (ent_off <= FAT_data.bytes_per_sector - 4u) ? 1u : 2u;

    unsigned char* buf = DSK_read_sectors(fat_sector, sec_count);
    if (!buf) return -1;

    uint32_t v = (uint32_t)buf[ent_off]
               | ((uint32_t)buf[ent_off + 1] << 8)
               | ((uint32_t)buf[ent_off + 2] << 16)
               | ((uint32_t)buf[ent_off + 3] << 24);

    free(buf);
    v &= 0x0FFFFFFF;
    return (int)v;
}

static int __write_fat_one(unsigned int fat_first_sector, unsigned int cluster, unsigned int value) {
    unsigned int fat_offset = cluster * 4u;
    unsigned int fat_sector = fat_first_sector + (fat_offset / FAT_data.bytes_per_sector);
    unsigned int ent_off    = fat_offset % FAT_data.bytes_per_sector;

    unsigned int sec_count = (ent_off <= FAT_data.bytes_per_sector - 4u) ? 1u : 2u;

    unsigned char* buf = DSK_read_sectors(fat_sector, sec_count);
    if (!buf) return -1;

    uint32_t oldv = (uint32_t)buf[ent_off]
                  | ((uint32_t)buf[ent_off + 1] << 8)
                  | ((uint32_t)buf[ent_off + 2] << 16)
                  | ((uint32_t)buf[ent_off + 3] << 24);

    uint32_t newv = (oldv & 0xF0000000u) | (value & 0x0FFFFFFFu);

    buf[ent_off]     = (unsigned char)(newv & 0xFF);
    buf[ent_off + 1] = (unsigned char)((newv >> 8) & 0xFF);
    buf[ent_off + 2] = (unsigned char)((newv >> 16) & 0xFF);
    buf[ent_off + 3] = (unsigned char)((newv >> 24) & 0xFF);

    int ok = (DSK_write_sectors(fat_sector, buf, sec_count) == 1) ? 0 : -1;
    free(buf);
    return ok;
}

static int __write_fat(unsigned int cluster, unsigned int value) {
    for (unsigned int i = 0; i < FAT_data.table_count; i++) {
        unsigned int fat_i_first = FAT_data.first_fat_sector + i * FAT_data.fat_size;
        if (__write_fat_one(fat_i_first, cluster, value) != 0) return -1;
    }
    return 0;
}









static inline int _is_cluster_free(unsigned int cluster) {
	if (!cluster) return 1;
	return 0;
}

static inline int _set_cluster_free(unsigned int cluster) {
	return __write_fat(cluster, 0);
}

static int _is_cluster_end(unsigned int cluster, int fatType) {
    (void)fatType;
    if (FAT_data.fat_type == 32) return (cluster >= END_CLUSTER_32);
    return 0;
}


static inline int _set_cluster_end(unsigned int cluster, int fatType) {
	if (FAT_data.fat_type == 32) return __write_fat(cluster, END_CLUSTER_32);
	return -1;
}

static inline int _is_cluster_bad(unsigned int cluster, int fatType) {
	if ((cluster == BAD_CLUSTER_32 && FAT_data.fat_type == 32)) return 1;
	return 0;
}

static inline int _set_cluster_bad(unsigned int cluster, int fatType) {
	if (FAT_data.fat_type == 32) return __write_fat(cluster, BAD_CLUSTER_32);
	if (FAT_data.fat_type == 16) return __write_fat(cluster, BAD_CLUSTER_16);
	if (FAT_data.fat_type == 12) return __write_fat(cluster, BAD_CLUSTER_12);
	return -1;
}

static unsigned int _cluster_allocate() {
    unsigned int max_cluster = FAT_data.total_clusters + 1; 
    if (max_cluster < 2) return 0;

    unsigned int start = last_allocated_cluster;
    if (start < 2 || start > max_cluster) start = 2;


    for (unsigned int c = start; c <= max_cluster; c++) {
        int st = __read_fat(c);
        if (st < 0) return 0;
        if ((unsigned int)st == FREE_CLUSTER_32) {
            if (_set_cluster_end(c, FAT_data.fat_type) == 0) {
                last_allocated_cluster = c + 1;
                if (last_allocated_cluster > max_cluster) last_allocated_cluster = 2;
                return c;
            }
            return 0;
        }
    }

    for (unsigned int c = 2; c < start; c++) {
        int st = __read_fat(c);
        if (st < 0) return 0;
        if ((unsigned int)st == FREE_CLUSTER_32) {
            if (_set_cluster_end(c, FAT_data.fat_type) == 0) {
                last_allocated_cluster = c + 1;
                if (last_allocated_cluster > max_cluster) last_allocated_cluster = 2;
                return c;
            }
            return 0;
        }
    }
    return 0;
}


static int _cluster_deallocate(const unsigned int cluster) {
	unsigned int cluster_status = __read_fat(cluster);
	if (_is_cluster_free(cluster_status) == 1) return 0;
	else if (cluster_status < 0) {
		printf("Function _cluster_deallocate: Error occurred with __read_fat, aborting operations...\n");
		return -1;
	}

	if (_set_cluster_free(cluster) == 0) return 0;
	else {
		printf("Function _cluster_deallocate: Error occurred with __write_fat, aborting operations...\n");
		return -1;
	}
}

static unsigned char* _cluster_readoff(unsigned int cluster, unsigned int offset) {
	unsigned int start_sect = (cluster - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	unsigned char* cluster_data = DSK_readoff_sectors(start_sect, offset, FAT_data.sectors_per_cluster);
	return cluster_data;
}

static unsigned char* _cluster_read(unsigned int cluster) {
	return _cluster_readoff(cluster, 0);
}

static unsigned char* _cluster_readoff_stop(unsigned int cluster, unsigned int offset, unsigned char* stop) {
	unsigned int start_sect = (cluster - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	unsigned char* cluster_data = DSK_readoff_sectors_stop(start_sect, offset, FAT_data.sectors_per_cluster, stop);
	return cluster_data;
}

static int _cluster_write(const unsigned char* data, unsigned int cluster) {
	unsigned int start_sect = (cluster - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	return (DSK_write_sectors(start_sect, data, FAT_data.sectors_per_cluster) == 1) ? 0 : -1;
}

static int _cluster_writeoff(const unsigned char* data, unsigned int cluster, unsigned int offset, unsigned int size) {
	unsigned int start_sect = (cluster - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	return (DSK_writeoff_sectors(start_sect, data, FAT_data.sectors_per_cluster, offset, size) == 1) ? 0 : -1;
}

static int _copy_cluster2cluster(unsigned int source, unsigned int destination) {
	unsigned int first = (source - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	unsigned int second = (destination - 2) * (unsigned short)FAT_data.sectors_per_cluster + FAT_data.first_data_sector;
	return DSK_copy_sectors2sectors(first, second, FAT_data.sectors_per_cluster);
}

static void _add_cluster_to_content(int ci) {
    Content* c = FAT_get_content_from_table(ci);
    if (!c || !c->file || !c->file->data || c->file->data_size <= 0) return;

    unsigned int last = c->file->data[c->file->data_size - 1];

    unsigned int newc = _cluster_allocate();
    if (newc == 0) return;

    if (__write_fat(last, newc) != 0) return;
    _set_cluster_end(newc, FAT_data.fat_type);

    unsigned int* nd = realloc(c->file->data, (c->file->data_size + 1) * sizeof(unsigned int));
    if (!nd) return;

    nd[c->file->data_size] = newc;
    c->file->data = nd;
    c->file->data_size += 1;

    unsigned char* zero = calloc(1, FAT_data.sectors_per_cluster * SECTOR_SIZE);
    if (zero) {
        _cluster_write(zero, newc);
        free(zero);
    }
}


static int _cluster_read_range(unsigned int cluster, unsigned int offset, unsigned char* out, unsigned int size) {
    unsigned int cluster_bytes = FAT_data.sectors_per_cluster * SECTOR_SIZE;
    if (offset + size > cluster_bytes) return -1;

    unsigned int sector_off = offset / SECTOR_SIZE;
    unsigned int byte_off   = offset % SECTOR_SIZE;

    unsigned int need_sectors = (byte_off + size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (sector_off + need_sectors > FAT_data.sectors_per_cluster) return -1;

    unsigned int lba = (cluster - 2) * FAT_data.sectors_per_cluster + FAT_data.first_data_sector + sector_off;

    unsigned char* tmp = DSK_readoff_sectors(lba, byte_off, need_sectors);
    if (!tmp) return -1;

    memcpy(out, tmp, size);
    free(tmp);
    return 0;
}


int FAT_directory_list(int ci, unsigned char attrs, int exclusive) {
	unsigned int cluster = GET_CLUSTER_FROM_ENTRY(FAT_get_content_from_table(ci)->meta, FAT_data.fat_type);
	
	Content* content = FAT_create_content();
	if (!content) return 0;

	content->directory = _create_directory();
	if (!content->directory) return 0;

	content->parent_cluster = 0;
	content->content_type = CONTENT_TYPE_DIRECTORY;

	const unsigned char default_hidden_attributes = (FILE_HIDDEN | FILE_SYSTEM);
	unsigned char attributes_to_hide = default_hidden_attributes;
	if (exclusive == 0) attributes_to_hide &= (~attrs);
	else if (exclusive == 1) attributes_to_hide = (~attrs);

	unsigned char* cluster_data = _cluster_read(cluster);
	if (cluster_data == NULL) {
		printf("Function FAT_directory_list: _cluster_read encountered an error. Aborting...\n");
		FAT_unload_content_system(content);
		return -1;
	}

	directory_entry_t* file_metadata = (directory_entry_t*)cluster_data;
	unsigned int meta_pointer_iterator_count = 0;
	while (1) {
		if (file_metadata->file_name[0] == ENTRY_END) break;
		else if (strncmp((char*)file_metadata->file_name, "..", 2) == 0 ||
				strncmp((char*)file_metadata->file_name, ".", 1) == 0) {
			file_metadata++;
			meta_pointer_iterator_count++;
		}
		else if (((file_metadata->file_name)[0] == ENTRY_FREE) || ((file_metadata->attributes & FILE_LONG_NAME) == FILE_LONG_NAME)) {	
			if (meta_pointer_iterator_count < FAT_data.cluster_size / sizeof(directory_entry_t) - 1) {
				file_metadata++;
				meta_pointer_iterator_count++;
			}
			else {
				unsigned int next_cluster = __read_fat(cluster);
				if (_is_cluster_end(next_cluster, FAT_data.fat_type) == 1) break;
				else if (next_cluster < 0) {
					printf("Function FAT_directory_list: __read_fat encountered an error. Aborting...\n");
					FAT_unload_content_system(content);
					return -1;
				}
				else {
					FAT_unload_content_system(content);
					return FAT_directory_list(next_cluster, attrs, exclusive);
				}
			}
		}
		else {
			if ((file_metadata->attributes & FILE_DIRECTORY) != FILE_DIRECTORY) {			
				File* file = _create_file();

				char name[13] = { 0 };
				strcpy(name, (const char*)file_metadata->file_name);
				strcpy(file->name, (const char*)strtok(name, " "));
				strcpy(file->extension, (const char*)strtok(NULL, " "));

				if (content->directory->files == NULL) content->directory->files = file;
				else {
					File* current = content->directory->files;
					while (current->next != NULL) current = current->next;
					current->next = file;
				}
			}
			else {
				if ((file_metadata->attributes & FILE_DIRECTORY) == FILE_DIRECTORY) {
					Directory* upperDir = _create_directory();

					char name[13] = { 0 };
					strcpy(name, (char*)file_metadata->file_name);
					strncpy(upperDir->name, strtok(name, " "), 11);
					
					if (content->directory->subDirectory == NULL) content->directory->subDirectory = upperDir;
					else {
						Directory* current = content->directory->subDirectory;
						while (current->next != NULL) current = current->next;
						current->next = upperDir;
					}
				}
			}

			file_metadata++;
			meta_pointer_iterator_count++;
		}
	}

	int root_ci = _add_content2table(content);
	if (root_ci == -1) {
		printf("Function FAT_open_content: an error occurred in _add_content2table. Aborting...\n");
		FAT_unload_content_system(content);
		return -1;
	}

	return root_ci;
}

static int _directory_search(const char* filepart, const unsigned int cluster, directory_entry_t* file, unsigned int* entryOffset) {
	char searchName[13] = { 0 };
	strcpy(searchName, filepart);
	if (_name_check(searchName) != 0)
		_name2fatname(searchName);

	unsigned char* cluster_data = _cluster_read(cluster);
	if (cluster_data == NULL) {
		printf("Function _directory_search: _cluster_read encountered an error. Aborting...\n");
		return -1;
	}

	directory_entry_t* file_metadata = (directory_entry_t*)cluster_data;
	unsigned int meta_pointer_iterator_count = 0;
	while (1) {
		if (file_metadata->file_name[0] == ENTRY_END) break;
		else if (strncmp((char*)file_metadata->file_name, searchName, 11) != 0) {
			if (meta_pointer_iterator_count < FAT_data.cluster_size / sizeof(directory_entry_t) - 1) {
				file_metadata++;
				meta_pointer_iterator_count++;
			}
			else {
				int next_cluster = __read_fat(cluster);
				if (_is_cluster_end(next_cluster, FAT_data.fat_type) == 1) break;
				else if (next_cluster < 0) {
					printf("Function _directory_search: __read_fat encountered an error. Aborting...\n");
					free(cluster_data);
					return -1;
				} 
				else {
					free(cluster_data);
					return _directory_search(filepart, next_cluster, file, entryOffset);
				}
			}
		}
		else {
			if (file != NULL) memcpy(file, file_metadata, sizeof(directory_entry_t));
			if (entryOffset != NULL) *entryOffset = meta_pointer_iterator_count;

			free(cluster_data);
			return 0;
		}
	}

	free(cluster_data);
	return -2;
}

static int _directory_add(const unsigned int cluster, directory_entry_t* file_to_add) {
	unsigned char* cluster_data = _cluster_read(cluster);
	if (cluster_data == NULL) {
		printf("Function _directory_add: _cluster_read encountered an error. Aborting...\n");
		return -1;
	}

	directory_entry_t* file_metadata = (directory_entry_t*)cluster_data;
	unsigned int meta_pointer_iterator_count = 0;
	while (1) {
		if (file_metadata->file_name[0] != ENTRY_FREE && file_metadata->file_name[0] != ENTRY_END) {
			if (meta_pointer_iterator_count < FAT_data.cluster_size / sizeof(directory_entry_t) - 1) {
				file_metadata++;
				meta_pointer_iterator_count++;
			}
			else {
				unsigned int next_cluster = __read_fat(cluster);
				if (_is_cluster_end(next_cluster, FAT_data.fat_type) == 1) {
					next_cluster = _cluster_allocate();
					if (_is_cluster_bad(next_cluster, FAT_data.fat_type) == 1) {
						printf("Function _directory_add: allocation of new cluster failed. Aborting...\n");
						free(cluster_data);
						return -1;
					}

					if (__write_fat(cluster, next_cluster) != 0) {
						printf("Function _directory_add: extension of the cluster chain with new cluster failed. Aborting...\n");
						free(cluster_data);
						return -1;
					}
				}

				free(cluster_data);
				return _directory_add(next_cluster, file_to_add);
			}
		}
		else {
			file_to_add->creation_date 			= DTM_current_date();
			file_to_add->creation_time 			= DTM_current_time();
			file_to_add->creation_time_tenths 	= DTM_current_time();
			file_to_add->last_accessed 			= file_to_add->creation_date;
			file_to_add->last_modification_date = file_to_add->creation_date;
			file_to_add->last_modification_time = file_to_add->creation_time;

			unsigned int new_cluster = _cluster_allocate();
			if (_is_cluster_bad(new_cluster, FAT_data.fat_type) == 1) {
				printf("Function _directory_add: allocation of new cluster failed. Aborting...\n");
				free(cluster_data);

				return -1;
			}
			
			file_to_add->low_bits  = GET_ENTRY_LOW_BITS(new_cluster, FAT_data.fat_type);
			file_to_add->high_bits = GET_ENTRY_HIGH_BITS(new_cluster, FAT_data.fat_type);

			memcpy(file_metadata, file_to_add, sizeof(directory_entry_t));
			if (_cluster_write(cluster_data, cluster) != 0) {
				printf("Function _directory_add: Writing new directory entry failed. Aborting...\n");
				free(cluster_data);
				return -1;
			}

			free(cluster_data);
			return 0;
		}
	}

	free(cluster_data);
	return -1; //return error.
}

static int _directory_edit(const unsigned int cluster, directory_entry_t* old_meta, const char* new_name) {
	if (_name_check((char*)old_meta->file_name) != 0) {
		printf("Function _directory_edit: Invalid file name!");
		return -1;
	}

	unsigned char* cluster_data = _cluster_read(cluster);
	if (cluster_data == NULL) {
		printf("Function _directory_edit: _cluster_read encountered an error. Aborting...\n");
		return -1;
	}

	directory_entry_t* file_metadata = (directory_entry_t*)cluster_data;
	unsigned int meta_pointer_iterator_count = 0;
	while (1) {
		if (memcmp(file_metadata->file_name, old_meta->file_name, 11) == 0) {

			old_meta->last_accessed = DTM_current_date();
			old_meta->last_modification_date = DTM_current_date();
			old_meta->last_modification_time = DTM_current_time();

			memset(old_meta->file_name, 0, 11);
			strncpy((char*)old_meta->file_name, new_name, 11);
			memcpy(file_metadata, old_meta, sizeof(directory_entry_t));
			
			if (_cluster_write(cluster_data, cluster) != 0) {
				printf("Function _directory_edit: Writing updated directory entry failed. Aborting...\n");
				free(cluster_data);
				return -1;
			}

			return 0;
		} 
		
		else if (meta_pointer_iterator_count < FAT_data.cluster_size / sizeof(directory_entry_t) - 1)  {
			file_metadata++;
			meta_pointer_iterator_count++;
		} 
		
		else {
			unsigned int next_cluster = __read_fat(cluster);
			if ((next_cluster >= END_CLUSTER_32 && FAT_data.fat_type == 32) || (next_cluster >= END_CLUSTER_16 && FAT_data.fat_type == 16) || (next_cluster >= END_CLUSTER_12 && FAT_data.fat_type == 12)) {
				printf("Function _directory_edit: End of cluster chain reached. File not found. Aborting...\n");
				free(cluster_data);
				return -2;
			}

			free(cluster_data);
			return _directory_edit(next_cluster, old_meta, new_name);
		}
	}

	free(cluster_data);
	return -1;
}

static int _directory_remove(const unsigned int cluster, const char* fileName) {
	if (_name_check(fileName) != 0) {
		printf("Function _directory_remove: Invalid file name!");
		return -1;
	}

	unsigned char* cluster_data = _cluster_read(cluster);
	if (cluster_data == NULL) {
		printf("Function _directory_remove: _cluster_read encountered an error. Aborting...\n");
		return -1;
	}

	directory_entry_t* file_metadata = (directory_entry_t*)cluster_data;
	unsigned int meta_pointer_iterator_count = 0;
	while (1) {
		if (memcmp(file_metadata->file_name, fileName, 11) == 0) {
			file_metadata->file_name[0] = ENTRY_FREE;
			if (_cluster_write(cluster_data, cluster) != 0) {
				printf("Function _directory_remove: Writing updated directory entry failed. Aborting...\n");
				free(cluster_data);
				return -1;
			}

			return 0;
		} 
		else if (meta_pointer_iterator_count < FAT_data.cluster_size / sizeof(directory_entry_t) - 1)  {
			file_metadata++;
			meta_pointer_iterator_count++;
		} 
		else {
			unsigned int next_cluster = __read_fat(cluster);
			if ((next_cluster >= END_CLUSTER_32 && FAT_data.fat_type == 32) || (next_cluster >= END_CLUSTER_16 && FAT_data.fat_type == 16) || (next_cluster >= END_CLUSTER_12 && FAT_data.fat_type == 12)) {
				printf("Function _directory_remove: End of cluster chain reached. File not found. Aborting...\n");
				free(cluster_data);
				return -2;
			}

			free(cluster_data);
			return _directory_remove(next_cluster, fileName);
		}
	}

	free(cluster_data);
	return -1; // Return error
}

int FAT_content_exists(const char* path) {
	char fileNamePart[256] = { 0 };
	unsigned short start = 0;
	unsigned int active_cluster = 0;

	if (FAT_data.fat_type == 32) active_cluster = FAT_data.ext_root_cluster;
	else {
		printf("Function FAT_content_exists: FAT16 and FAT12 are not supported!\n");
		return -1;
	}

	directory_entry_t file_info;
	for (unsigned int iterator = 0; iterator <= strlen(path); iterator++) {
		if (path[iterator] == '\\' || path[iterator] == '\0') {
			memset(fileNamePart, '\0', 256);
			memcpy(fileNamePart, path + start, iterator - start);

			int result = _directory_search(fileNamePart, active_cluster, &file_info, NULL);
			if (result != 0) return 0;

			start = iterator + 1;
			active_cluster = GET_CLUSTER_FROM_ENTRY(file_info, FAT_data.fat_type);
		}
	}

	return 1; // Content exists
}

int FAT_open_content(const char* path) {
	Content* fat_content = FAT_create_content();
	if (!fat_content) return -1;

	char fileNamePart[256] = { 0 };
	unsigned short start = 0;
	unsigned int active_cluster = 0;

	if (FAT_data.fat_type == 32) active_cluster = FAT_data.ext_root_cluster;
	else {
		printf("Function FAT_open_content: FAT16 and FAT12 are not supported!\n");
		FAT_unload_content_system(fat_content);
		return -2;
	}
	
	directory_entry_t content_meta;
	unsigned int parent = active_cluster;

	for (unsigned int i = 0; i <= strlen(path); i++) {
	    if (path[i] == '\\' || path[i] == '\0') {
	        if (i == start) {
	            start = i + 1;
	            continue;
	        }

	        memset(fileNamePart, 0, sizeof(fileNamePart));
	        memcpy(fileNamePart, path + start, i - start);

	        parent = active_cluster;

	        int result = _directory_search(fileNamePart, active_cluster, &content_meta, NULL);
	        if (result == -2) { FAT_unload_content_system(fat_content); return -3; }
	        if (result == -1) { FAT_unload_content_system(fat_content); return -4; }

	        unsigned int entry_cluster = GET_CLUSTER_FROM_ENTRY(content_meta, FAT_data.fat_type);

	        if (path[i] == '\0') {
	            fat_content->parent_cluster = parent;
	        }

	        active_cluster = entry_cluster;
	        start = i + 1;
	    }
	}


	memcpy(&fat_content->meta, &content_meta, sizeof(directory_entry_t));
	if ((content_meta.attributes & FILE_DIRECTORY) != FILE_DIRECTORY) {
		fat_content->file = _create_file();
		if (!fat_content->file) {
			FAT_unload_content_system(fat_content);
			return -5;
		}

		fat_content->content_type = CONTENT_TYPE_FILE;
		unsigned int* content = NULL;
		int content_size = 0;
		
		int cluster = GET_CLUSTER_FROM_ENTRY(content_meta, FAT_data.fat_type);
		while (cluster < END_CLUSTER_32) {
			unsigned int* new_content = (unsigned int*)realloc(content, (content_size + 1) * sizeof(unsigned int));
			if (new_content == NULL) {
				free(content);
				FAT_unload_content_system(fat_content);
				return -6;
			}

			new_content[content_size] = cluster;
			content = new_content;
			content_size++;

			cluster = __read_fat(cluster);
			if (cluster == BAD_CLUSTER_32) {
				printf("Function FAT_open_content: the cluster chain is corrupted with a bad cluster. Aborting...\n");
				free(content);
				FAT_unload_content_system(fat_content);
				return -7;
			}
			else if (cluster == -1) {
				printf("Function FAT_open_content: an error occurred in __read_fat. Aborting...\n");
				free(content);
				FAT_unload_content_system(fat_content);
				return -8;
			}
		}
		
		fat_content->file->data = (unsigned int*)malloc(content_size * sizeof(unsigned int));
		if (!fat_content->file->data) {
			free(content);
			free(fat_content->file);
			FAT_unload_content_system(fat_content);
			return -9;
		}

		memcpy(fat_content->file->data, content, content_size * sizeof(unsigned int));
		fat_content->file->data_size = content_size;
		free(content);

		char full[13] = {0};
		_fatname2name((char*)fat_content->meta.file_name, full);   // например "TEST_1.TXT" или "ASD"

		fat_content->file->name[0] = '\0';
		fat_content->file->extension[0] = '\0';

		char* dot = strchr(full, '.');
		if (dot) {
		    *dot = '\0';
		
		    strncpy(fat_content->file->name, full, sizeof(fat_content->file->name) - 1);
		    fat_content->file->name[sizeof(fat_content->file->name) - 1] = '\0';
		
		    strncpy(fat_content->file->extension, dot + 1, sizeof(fat_content->file->extension) - 1);
		    fat_content->file->extension[sizeof(fat_content->file->extension) - 1] = '\0';
		} else {
		    strncpy(fat_content->file->name, full, sizeof(fat_content->file->name) - 1);
		    fat_content->file->name[sizeof(fat_content->file->name) - 1] = '\0';
		    fat_content->file->extension[0] = '\0';
		}



	}
	else {
		fat_content->directory = _create_directory();
		if (!fat_content->directory) {
			FAT_unload_content_system(fat_content);
			return -10;
		}

		fat_content->content_type = CONTENT_TYPE_DIRECTORY;
		char dname[13] = {0};
		_fatname2name((char*)content_meta.file_name, dname);
		char* dot = strchr(dname, '.');
		if (dot) *dot = '\0';
			
		strncpy(fat_content->directory->name, dname, sizeof(fat_content->directory->name) - 1);
		fat_content->directory->name[sizeof(fat_content->directory->name) - 1] = '\0';
	}

	int ci = _add_content2table(fat_content);
	if (ci < 0) {
		printf("Function FAT_open_content: an error occurred in _add_content2table. Aborting...\n");
		if (fat_content->file) free(fat_content->file);
		else if (fat_content->directory) free(fat_content->directory);
		FAT_unload_content_system(fat_content);
		return -11;
	}

	return ci;
}

Content* FAT_get_content_from_table(int ci) {
	return _content_table[ci];
}

int FAT_close_content(int ci) {
	return _remove_content_from_table(ci);
}

// Function for reading part of file
// data - content for reading
// buffer - buffer data storage
// offset - file seek
// size - size of read data
int FAT_read_content2buffer(int ci, unsigned char* buffer, unsigned int offset, unsigned int size) {
    Content* c = FAT_get_content_from_table(ci);
    if (!c || !c->file) return -1;

    unsigned int file_size = c->meta.file_size;
    if (offset >= file_size) return 0;

    unsigned int to_read = size;
    if (to_read > file_size - offset) to_read = file_size - offset;

    unsigned int cluster_bytes = FAT_data.sectors_per_cluster * SECTOR_SIZE;

    unsigned int cluster_seek   = offset / cluster_bytes;
    unsigned int in_cluster_off = offset % cluster_bytes;

    unsigned int pos = 0;
    while (pos < to_read && cluster_seek < (unsigned int)c->file->data_size) {
        unsigned int chunk = to_read - pos;
        unsigned int max_in_cluster = cluster_bytes - in_cluster_off;
        if (chunk > max_in_cluster) chunk = max_in_cluster;

        if (_cluster_read_range(c->file->data[cluster_seek], in_cluster_off, buffer + pos, chunk) != 0) {
            return (int)pos; // что успели
        }

        pos += chunk;
        cluster_seek++;
        in_cluster_off = 0;
    }

    return (int)pos;
}



int FAT_write_buffer2content(int ci, const unsigned char* buffer, unsigned int offset, unsigned int size) {
    Content* c = FAT_get_content_from_table(ci);
    if (!c || !c->file) return -1;

    unsigned int cluster_bytes = FAT_data.sectors_per_cluster * SECTOR_SIZE;

    unsigned int cluster_seek   = offset / cluster_bytes;
    unsigned int in_cluster_off = offset % cluster_bytes;

    while ((unsigned int)c->file->data_size <= cluster_seek) {
        _add_cluster_to_content(ci);
        if ((unsigned int)c->file->data_size <= cluster_seek) return -2;
    }

    unsigned int pos = 0;
    unsigned int idx = cluster_seek;

    while (pos < size) {
        if (idx >= (unsigned int)c->file->data_size) {
            _add_cluster_to_content(ci);
            if (idx >= (unsigned int)c->file->data_size) return (int)pos;
        }

        unsigned int chunk = size - pos;
        unsigned int max_in_cluster = cluster_bytes - in_cluster_off;
        if (chunk > max_in_cluster) chunk = max_in_cluster;

        if (_cluster_writeoff(buffer + pos, c->file->data[idx], in_cluster_off, chunk) != 0) {
            return (int)pos;
        }

        pos += chunk;
        idx++;
        in_cluster_off = 0;
    }

    // обновим логический размер файла
    unsigned int end_pos = offset + size;
    if (end_pos > c->meta.file_size) c->meta.file_size = end_pos;

    return 1;
}




int FAT_change_meta(const char* path, const char* new_name) {
	char fileNamePart[256] = { 0 };
	unsigned short start = 0;
	unsigned int active_cluster = 0;
	unsigned int prev_active_cluster = 0;

	if (FAT_data.fat_type == 32) active_cluster = FAT_data.ext_root_cluster;
	else {
		printf("Function FAT_change_meta: FAT16 and FAT12 are not supported!\n");
		return -1;
	}

	directory_entry_t file_info;
	if (strlen(path) == 0) {
		if (FAT_data.fat_type == 32) {
			active_cluster 		 = FAT_data.ext_root_cluster;
			file_info.attributes = FILE_DIRECTORY | FILE_VOLUME_ID;
			file_info.file_size  = 0;
			file_info.high_bits  = GET_ENTRY_HIGH_BITS(active_cluster, FAT_data.fat_type);
			file_info.low_bits 	 = GET_ENTRY_LOW_BITS(active_cluster, FAT_data.fat_type);
		}
		else {
			printf("Function FAT_change_meta: FAT16 and FAT12 are not supported!\n");
			return -1;
		}
	}
	else {
		for (unsigned int iterator = 0; iterator <= strlen(path); iterator++) 
			if (path[iterator] == '\\' || path[iterator] == '\0') {
				prev_active_cluster = active_cluster;

				memset(fileNamePart, '\0', 256);
				memcpy(fileNamePart, path + start, iterator - start);

				int retVal = _directory_search(fileNamePart, active_cluster, &file_info, NULL);
				switch (retVal) {
					case -2:
						printf("Function FAT_change_meta: No matching directory found. Aborting...\n");
					return -2;

					case -1:
						printf("Function FAT_change_meta: An error occurred in _directory_search. Aborting...\n");
					return retVal;
				}

				start = iterator + 1;
				active_cluster = GET_CLUSTER_FROM_ENTRY(file_info, FAT_data.fat_type); //prep for next search
			}
	}

	if (_directory_edit(prev_active_cluster, &file_info, new_name) != 0) {
		printf("Function FAT_change_meta: _directory_edit encountered an error. Aborting...\n");
		return -1;
	}
	
	return 0;
}

int FAT_put_content(const char* path, Content* content) {
    int parent_ci = FAT_open_content(path);

    if (parent_ci < 0) return parent_ci;

    Content* parent = FAT_get_content_from_table(parent_ci);
    if (!parent) {
        FAT_close_content(parent_ci);
        return -1;
    }

	if (parent->content_type != CONTENT_TYPE_DIRECTORY) {
        FAT_close_content(parent_ci);
        return -2;
    }

    directory_entry_t file_info = parent->meta;
    unsigned int active_cluster = GET_CLUSTER_FROM_ENTRY(file_info, FAT_data.fat_type);

    FAT_close_content(parent_ci);

    char output[13] = {0};
    _fatname2name((char*)content->meta.file_name, output);

    int retVal = _directory_search(output, active_cluster, NULL, NULL);
    if (retVal == -1) {
        printf("Function FAT_put_content: _directory_search error. Aborting...\n");
        return -1;
    } else if (retVal != -2) {
        printf("Function FAT_put_content: file already exists. Aborting...\n");
        return -3;
    }

    if (_directory_add(active_cluster, &content->meta) != 0) {
        printf("Function FAT_put_content: _directory_add error. Aborting...\n");
        return -1;
    }

    return 0;
}

int FAT_delete_content(const char* path) {
	int ci = FAT_open_content(path);
	Content* fat_content = FAT_get_content_from_table(ci);
	if (fat_content == NULL) {
		printf("Function FAT_delete_content: FAT_open_content encountered an error. Aborting...\n");
		return -1;
	}

	unsigned int data_cluster = GET_CLUSTER_FROM_ENTRY(fat_content->meta, FAT_data.fat_type);
	unsigned int prev_cluster = 0;
	
	while (data_cluster < END_CLUSTER_32) {
		prev_cluster = __read_fat(data_cluster);
		if (_cluster_deallocate(data_cluster) != 0) {
			printf("[%s %i] _cluster_deallocate encountered an error. Aborting...\n", __FILE__, __LINE__);
			_remove_content_from_table(ci);
			return -1;
		}

		data_cluster = prev_cluster;
	}

	if (_directory_remove(fat_content->parent_cluster, (char*)fat_content->meta.file_name) != 0) {
		printf("[%s %i] _directory_remove encountered an error. Aborting...\n", __FILE__, __LINE__);
		_remove_content_from_table(ci);
		return -1;
	}

	_remove_content_from_table(ci);
	return 0;
}

void FAT_copy_content(char* source, char* destination) {
	int ci_source = FAT_open_content(source);

	Content* fat_content = FAT_get_content_from_table(ci_source);
	Content* dst_content = NULL;

	directory_entry_t content_meta;
	memcpy(&content_meta, &fat_content->meta, sizeof(directory_entry_t));

	if (fat_content->directory != NULL) dst_content = FAT_create_object(fat_content->directory->name, 1, NULL);
	else if (fat_content->file != NULL) dst_content = FAT_create_object(fat_content->file->name, 0, fat_content->file->extension);

	directory_entry_t dst_meta;		
	memcpy(&dst_meta, &dst_content->meta, sizeof(directory_entry_t));

	int ci_destination = FAT_put_content(destination, dst_content);
	unsigned int data_cluster = GET_CLUSTER_FROM_ENTRY(content_meta, FAT_data.fat_type);
	unsigned int dst_cluster  = GET_CLUSTER_FROM_ENTRY(dst_meta, FAT_data.fat_type);

	while (data_cluster < END_CLUSTER_32) {
		_add_cluster_to_content(ci_destination);
		dst_cluster = __read_fat(dst_cluster);
		_copy_cluster2cluster(data_cluster, dst_cluster);
		data_cluster = __read_fat(data_cluster);
	}

	_remove_content_from_table(ci_destination);
	_remove_content_from_table(ci_source);
}

int FAT_stat_content(int ci, CInfo_t* info) {
	Content* content = FAT_get_content_from_table(ci);
	if (!content) {
		info->type = NOT_PRESENT;
		return -1;
	}

	if (content->content_type == CONTENT_TYPE_DIRECTORY) {
		info->size = 0;
		strcpy((char*)info->full_name, (char*)content->directory->name);
		info->type = STAT_DIR;
	}
	else if (content->content_type == CONTENT_TYPE_FILE) {
		info->size = content->meta.file_size;
		strcpy((char*)info->full_name, (char*)content->meta.file_name);
		strcpy(info->file_name, content->file->name);
		strcpy(info->file_extension, content->file->extension);
		info->type = STAT_FILE;
	}
	else {
		return -2;
	}

	info->creation_date = content->meta.creation_date;
	info->creation_time = content->meta.creation_time;
	info->last_accessed = content->meta.last_accessed;
	info->last_modification_date = content->meta.last_modification_date;
	info->last_modification_time = content->meta.last_modification_time;

	return 1;
}

int _add_content2table(Content* content) {
	for (int i = 0; i < CONTENT_TABLE_SIZE; i++) {
		if (!_content_table[i]) {
			_content_table[i] = content;
			return i;
		}
	}

	return -1;
}

int _remove_content_from_table(int index) {
	if (!_content_table[index]) return -1;
	int result = FAT_unload_content_system(_content_table[index]);
	_content_table[index] = NULL;
	return result;
}

void _fatname2name(char* input, char* output) {
	if (input[0] == '.') {
		if (input[1] == '.') {
			strcpy (output, "..");
			return;
		}

		strcpy (output, ".");
		return;
	}

	unsigned short counter = 0;
	for ( counter = 0; counter < 8; counter++) {
		if (input[counter] == 0x20) {
			output[counter] = '.';
			break;
		}

		output[counter] = input[counter];
	}

	if (counter == 8) 
		output[counter] = '.';

	unsigned short counter2 = 8;
	for (counter2 = 8; counter2 < 11; counter2++) {
		++counter;
		if (input[counter2] == 0x20 || input[counter2] == 0x20) {
			if (counter2 == 8)
				counter -= 2;

			break;
		}
		
		output[counter] = input[counter2];		
	}

	++counter;
	while (counter < 12) {
		output[counter] = ' ';
		++counter;
	}

	output[12] = '\0';
	return;
}

char* _name2fatname(char* input) {
	str2uppercase(input);

	int haveExt = 0;
	char searchName[13] = { '\0' };
	unsigned short dotPos = 0;
	unsigned int counter = 0;

	while (counter <= 8) {
		if (input[counter] == '.' || input[counter] == '\0') {
			if (input[counter] == '.') haveExt = 1;
			dotPos = counter;
			counter++;
			break;
		}
		else {
			searchName[counter] = input[counter];
			counter++;
		}
	}

	if (counter > 9) {
		counter = 8;
		dotPos = 8;
	}
	
	unsigned short extCount = 8;
	while (extCount < 11) {
		if (input[counter] != '\0' && haveExt == 1) searchName[extCount] = input[counter];
		else searchName[extCount] = ' ';

		counter++;
		extCount++;
	}

	counter = dotPos;
	while (counter < 8) {
		searchName[counter] = ' ';
		counter++;
	}

	strcpy(input, searchName);
	return input;
}

int _name_check(const char* input) {
	short retVal = 0;
	unsigned short iterator = 0;
	for (iterator = 0; iterator < 11; iterator++) {
		if (input[iterator] < 0x20 && input[iterator] != 0x05) {
			retVal = retVal | BAD_CHARACTER;
		}
		
		switch (input[iterator]) {
			case 0x2E: {
				if ((retVal & NOT_CONVERTED_YET) == NOT_CONVERTED_YET)
					retVal |= TOO_MANY_DOTS;

				retVal ^= NOT_CONVERTED_YET;
				break;
			}

			case 0x22:
			case 0x2A:
			case 0x2B:
			case 0x2C:
			case 0x2F:
			case 0x3A:
			case 0x3B:
			case 0x3C:
			case 0x3D:
			case 0x3E:
			case 0x3F:
			case 0x5B:
			case 0x5C:
			case 0x5D:
			case 0x7C:
				retVal = retVal | BAD_CHARACTER;
		}

		if (input[iterator] >= 'a' && input[iterator] <= 'z') {
			retVal = retVal | LOWERCASE_ISSUE;
		}
	}

	return retVal;
}

static directory_entry_t* _create_entry(const char* name, const char* ext, int isDir, unsigned int firstCluster, unsigned int filesize) {
	directory_entry_t* data = (directory_entry_t*)malloc(sizeof(directory_entry_t));
	if (!data) return NULL;

	data->reserved0 			 = 0; 
	data->creation_time_tenths 	 = 0;
	data->creation_time 		 = 0;
	data->creation_date 		 = 0;
	data->last_modification_date = 0;

	char* file_name = (char*)malloc(25);
	if (!file_name) {
		free(data);
		return NULL;
	}
	
	strcpy(file_name, name);
	if (ext) {
		strcat(file_name, ".");
		strcat(file_name, ext);
	}
	
	data->low_bits 	= firstCluster;
	data->high_bits = firstCluster >> 16;  

	if (isDir == 1) {
		data->file_size  = 0;
		data->attributes = FILE_DIRECTORY;
	} 
	else {
		data->file_size  = filesize;
		data->attributes = FILE_ARCHIVE;
	}

	data->creation_date = DTM_current_date();
	data->creation_time = DTM_current_time();
	data->creation_time_tenths = DTM_current_time();

	if (_name_check(file_name) != 0) _name2fatname(file_name);
	size_t n = strlen(file_name);
	if (n > 11) n = 11;
	strncpy((char*)data->file_name, file_name, n);
	free(file_name);

	return data; 
}

Content* FAT_create_object(char* name, int is_directory, char* extension) {
	Content* content = FAT_create_content();
	if (!content) return NULL;

	const char* ext = (extension && extension[0] != '\0') ? extension : NULL;
	if (strlen(name) > 11 || (ext && strlen(ext) > 4)) {
		printf("Uncorrect name or ext lenght.\n");
		FAT_unload_content_system(content);
		return NULL;
	}

	if (is_directory) {
		content->content_type = CONTENT_TYPE_DIRECTORY;
		content->directory = _create_directory();
		strcpy(content->directory->name, name);

		directory_entry_t* meta = _create_entry(name, NULL, 1, _cluster_allocate(), 0);
		if (meta) memcpy(&content->meta, meta, sizeof(directory_entry_t));
	} 
	else {
		content->content_type = CONTENT_TYPE_FILE;
		content->file = _create_file();

		strcpy(content->file->name, name);
		if (ext) strcpy(content->file->extension, ext);
		else content->file->extension[0] = '\0';

		directory_entry_t* meta = _create_entry(name, ext, 0, _cluster_allocate(), 1);
		if (meta) memcpy(&content->meta, meta, sizeof(directory_entry_t));
	}

	return content;
}

Content* FAT_create_content() {
	Content* content = (Content*)malloc(sizeof(Content));
	if (!content) return NULL;
	content->directory      = NULL;
	content->file           = NULL;
	content->parent_cluster = -1;
	return content;
}

Directory* _create_directory() {
	Directory* directory = (Directory*)malloc(sizeof(Directory));
	if (!directory) return NULL;
	directory->files        = NULL;
	directory->subDirectory = NULL;
	directory->next         = NULL;
	return directory;
}

File* _create_file() {
	File* file = (File*)malloc(sizeof(File));
	if (!file) return NULL;
	file->next = NULL;
	file->data = NULL;
	return file;
}

static int _unload_file_system(File* file) {
	if (!file) return -1;
	if (file->next) _unload_file_system(file->next);
	if (file->data) free(file->data);
	free(file);
	return 1;
}

static int _unload_directory_system(Directory* directory) {
	if (!directory) return -1;
	if (directory->files) _unload_file_system(directory->files);
	if (directory->subDirectory) _unload_directory_system(directory->subDirectory);
	if (directory->next) _unload_directory_system(directory->next);
	free(directory);
	return 1;
}

int FAT_unload_content_system(Content* content) {
	if (!content) return -1;
	if (content->content_type == CONTENT_TYPE_DIRECTORY) _unload_directory_system(content->directory);
	else if (content->content_type == CONTENT_TYPE_DIRECTORY) _unload_file_system(content->file);
	free(content);
	return 1;
}	
