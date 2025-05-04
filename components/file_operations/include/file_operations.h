#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include <stdbool.h>

// Directory operations
void list_files(const char *path);
char* list_files_json(const char *path);
void create_directory(const char *path);
void delete_path(const char *path);

// File operations
void copy_file(const char *src_path, const char *dest_path);
void move_file(const char *old_path, const char *new_path);
size_t get_file_size(const char *path);
char* read_file_content(const char *path);
int count_files_in_directory(const char *path);
bool sd_card_dir_exists(const char *path);

#endif // FILE_OPS_H