/**
 * @file file_operations.c
 * @brief File system operations for SD card management
 * 
 * This file contains implementations for various file operations including:
 * - Directory listing (text and JSON formats)
 * - File/directory creation, deletion, copying, moving
 * - File content reading
 * - Directory existence checking
 */

 #include "file_operations.h"
 #include "i2s_recorder_main.h"
 #include <ctype.h>
 #include <time.h>
 
 static const char *TAG = "file_operations";
 
 /**
  * @brief Lists files in a directory with basic formatting
  * @param path The directory path to list (e.g., "/sdcard/recordings")
  * 
  * @example 
  * list_files("/sdcard"); 
  * // Output:
  * // Contents of '/sdcard':
  * // Name                          Size       Type
  * // recordings                    4096       DIR
  * // config.txt                    128        FILE
  */
 void list_files(const char *path) {
     mount_sdcard();
 
     DIR *dir = opendir(path);
     if (!dir) {
         ESP_LOGE("LS", "Failed to open directory: %s", path);
         return;
     }
 
     struct dirent *entry;
     struct stat entry_stat;
     char full_path[512];
 
     printf("Contents of '%s':\n", path);
     printf("%-30s %-10s %s\n", "Name", "Size", "Type");
 
     #pragma GCC diagnostic push
     #pragma GCC diagnostic ignored "-Wformat-truncation"
     while ((entry = readdir(dir)) != NULL) {
         snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
     #pragma GCC diagnostic pop
 
         if (stat(full_path, &entry_stat) == -1) {
             continue;
         }
 
         printf("%-30s %-10ld %s\n", 
                entry->d_name,
                entry_stat.st_size,
                S_ISDIR(entry_stat.st_mode) ? "DIR" : "FILE");
     }
 
     closedir(dir);
 }
 
 /**
  * @brief Escapes special characters in a string for JSON output
  * @param dest Destination buffer for escaped string
  * @param dest_size Size of destination buffer
  * @param src Source string to escape
  * 
  * @note Handles all JSON special characters and non-printable characters
  */
 static void json_escape_string(char *dest, size_t dest_size, const char *src) {
     size_t i = 0, j = 0;
     while (src[i] != '\0' && j < dest_size - 1) {
         switch (src[i]) {
             case '"':  if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = '"'; } break;
             case '\\': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = '\\'; } break;
             case '\b': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'b'; } break;
             case '\f': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'f'; } break;
             case '\n': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'n'; } break;
             case '\r': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'r'; } break;
             case '\t': if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 't'; } break;
             default:
                 if (isprint((unsigned char)src[i])) {
                     dest[j++] = src[i];
                 } else if (j < dest_size - 6) {
                     sprintf(dest + j, "\\u%04x", (unsigned char)src[i]);
                     j += 6;
                 }
                 break;
         }
         i++;
     }
     dest[j] = '\0';
 }
 
 /**
  * @brief Dynamically appends a string to a JSON buffer with reallocation
  * @param buffer Pointer to the buffer pointer
  * @param buffer_size Pointer to buffer size variable
  * @param offset Pointer to current offset in buffer
  * @param str String to append
  * @return true on success, false on memory allocation failure
  */
 static bool append_json_string(char **buffer, size_t *buffer_size, size_t *offset, const char *str) {
     size_t needed = *offset + strlen(str) + 1;
     
     if (needed > *buffer_size) {
         size_t new_size = *buffer_size * 2;
         while (new_size < needed) new_size *= 2;
         
         char *new_buffer = realloc(*buffer, new_size);
         if (!new_buffer) return false;
         
         *buffer = new_buffer;
         *buffer_size = new_size;
     }
     
     strcpy(*buffer + *offset, str);
     *offset += strlen(str);
     return true;
 }
 
 /**
  * @brief Generates a JSON string listing directory contents
  * @param path Directory path to list
  * @return Dynamically allocated JSON string, or NULL on error
  * 
  * @example Output format:
  * {
  *   "path":"/sdcard",
  *   "files":[
  *     {"name":"file1.txt","path":"/sdcard/file1.txt","size":128,"type":"file","modified":"2023-01-01 12:00:00"},
  *     {"name":"folder1","path":"/sdcard/folder1","size":4096,"type":"directory","modified":"2023-01-01 12:05:00"}
  *   ]
  * }
  */
 char* list_files_json(const char *path) {
     if (!path || path[0] == '\0') {
         return NULL;
     }
 
     DIR *dir = opendir(path);
     if (!dir) {
         ESP_LOGE(TAG, "Failed to open directory: %s", path);
         return NULL;
     }
 
     size_t buf_size = 1024;
     char *json_buffer = malloc(buf_size);
     if (!json_buffer) {
         closedir(dir);
         return NULL;
     }
 
     char escaped_path[1024];
     json_escape_string(escaped_path, sizeof(escaped_path), path);
     size_t offset = snprintf(json_buffer, buf_size, 
                           "{\"path\":\"%s\",\"files\":[", escaped_path);
 
     struct dirent *entry;
     struct stat entry_stat;
     bool first_entry = true;
 
     while ((entry = readdir(dir)) != NULL) {
         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
             continue;
         }
 
         char full_path[1024];
         snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
 
         if (stat(full_path, &entry_stat) == -1) {
             continue;
         }
 
         char escaped_name[1024];
         char escaped_full_path[2048];
         json_escape_string(escaped_name, sizeof(escaped_name), entry->d_name);
         json_escape_string(escaped_full_path, sizeof(escaped_full_path), full_path);
 
         char time_buf[64];
         struct tm *tm_info = localtime(&entry_stat.st_mtime);
         strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
 
         size_t needed = offset + 512;
         if (needed > buf_size) {
             buf_size *= 2;
             char *new_buf = realloc(json_buffer, buf_size);
             if (!new_buf) {
                 free(json_buffer);
                 closedir(dir);
                 return NULL;
             }
             json_buffer = new_buf;
         }
 
         offset += snprintf(json_buffer + offset, buf_size - offset,
             "%s{\"name\":\"%s\",\"path\":\"%s\",\"size\":%ld,"
             "\"type\":\"%s\",\"modified\":\"%s\"}",
             first_entry ? "" : ",",
             escaped_name,
             escaped_full_path,
             entry_stat.st_size,
             S_ISDIR(entry_stat.st_mode) ? "directory" : "file",
             time_buf);
 
         first_entry = false;
     }
 
     if (offset + 3 > buf_size) {
         buf_size = offset + 3;
         char *new_buf = realloc(json_buffer, buf_size);
         if (!new_buf) {
             free(json_buffer);
             closedir(dir);
             return NULL;
         }
         json_buffer = new_buf;
     }
     strcat(json_buffer, "]}");
     closedir(dir);
 
     return json_buffer;
 }
 
 /**
  * @brief Creates a new directory
  * @param path Full path of directory to create
  * 
  * @example create_directory("/sdcard/new_folder");
  */
 void create_directory(const char *path) {
     mount_sdcard();
     
     int ret = mkdir(path, 0777);
     if (ret == -1) {
         ESP_LOGE("MKDIR", "Failed to create directory %s: %s", path, strerror(errno));
     } else {
         ESP_LOGI("MKDIR", "Created directory: %s", path);
     }
 }
 
 /**
  * @brief Recursively deletes a file or directory
  * @param path Path to delete
  * 
  * @example delete_path("/sdcard/old_folder");
  */
 void delete_path(const char *path) {
     mount_sdcard();
     
     struct stat path_stat;
     if (stat(path, &path_stat)) {
         ESP_LOGE("DELETE", "Path doesn't exist: %s", path);
         return;
     }
 
     if (S_ISDIR(path_stat.st_mode)) {
         DIR *dir = opendir(path);
         struct dirent *entry;
         
         while ((entry = readdir(dir)) != NULL) {
             if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                 continue;
             }
             
             char full_path[512];
             snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
             delete_path(full_path);
         }
         closedir(dir);
         
         if (rmdir(path)) {
             ESP_LOGE("DELETE", "Failed to remove directory %s: %s", path, strerror(errno));
         }
     } else {
         if (unlink(path)) {
             ESP_LOGE("DELETE", "Failed to delete file %s: %s", path, strerror(errno));
         }
     }
 }
 
 /**
  * @brief Copies a file from source to destination
  * @param src_path Source file path
  * @param dest_path Destination file path
  * 
  * @example copy_file("/sdcard/file1.txt", "/sdcard/backups/file1.txt");
  */
 void copy_file(const char *src_path, const char *dest_path) {
     mount_sdcard();
     
     FILE *src = fopen(src_path, "rb");
     if (!src) {
         ESP_LOGE("COPY", "Failed to open source file %s: %s", src_path, strerror(errno));
         return;
     }
 
     FILE *dest = fopen(dest_path, "wb");
     if (!dest) {
         fclose(src);
         ESP_LOGE("COPY", "Failed to open destination file %s: %s", dest_path, strerror(errno));
         return;
     }
 
     char buffer[1024];
     size_t bytes_read;
     while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
         fwrite(buffer, 1, bytes_read, dest);
     }
 
     fclose(src);
     fclose(dest);
     ESP_LOGI("COPY", "Copied %s to %s", src_path, dest_path);
 }
 
 /**
  * @brief Moves (renames) a file or directory
  * @param old_path Original path
  * @param new_path New path
  * 
  * @example move_file("/sdcard/temp.txt", "/sdcard/permanent.txt");
  */
 void move_file(const char *old_path, const char *new_path) {
     mount_sdcard();
     
     if (rename(old_path, new_path)) {
         ESP_LOGE("MOVE", "Failed to move %s to %s: %s", old_path, new_path, strerror(errno));
     } else {
         ESP_LOGI("MOVE", "Moved %s to %s", old_path, new_path);
     }
 }
 
 /**
  * @brief Gets the size of a file in bytes
  * @param path Path to the file
  * @return File size in bytes, or 0 on error
  * 
  * @example size_t size = get_file_size("/sdcard/data.bin");
  */
 size_t get_file_size(const char *path) {
     mount_sdcard();
     
     struct stat st;
     if (stat(path, &st)) {
         ESP_LOGE("SIZE", "Failed to get size of %s: %s", path, strerror(errno));
         return 0;
     }
     return st.st_size;
 }
 
 /**
  * @brief Reads entire file content into memory
  * @param path Path to the file
  * @return Dynamically allocated buffer with file content (must be freed by caller)
  * 
  * @example 
  * char *content = read_file_content("/sdcard/config.txt");
  * if (content) { free(content); }
  */
 char* read_file_content(const char *path) {
     mount_sdcard();
     
     FILE *file = fopen(path, "rb");
     if (!file) {
         ESP_LOGE("READ", "Failed to open file %s: %s", path, strerror(errno));
         return NULL;
     }
 
     fseek(file, 0, SEEK_END);
     long size = ftell(file);
     fseek(file, 0, SEEK_SET);
 
     char *content = malloc(size + 1);
     if (!content) {
         fclose(file);
         ESP_LOGE("READ", "Memory allocation failed");
         return NULL;
     }
 
     fread(content, 1, size, file);
     content[size] = '\0';
     fclose(file);
 
     return content;
 }
 
 /**
  * @brief Counts files (not directories) in a directory
  * @param path Directory path
  * @return Number of files, or -1 on error
  * 
  * @example int count = count_files_in_directory("/sdcard/recordings");
  */
 int count_files_in_directory(const char *path) {
     mount_sdcard();
 
     DIR *dir = opendir(path);
     if (!dir) {
         ESP_LOGE("COUNT", "Failed to open directory: %s", path);
         return -1;
     }
 
     struct dirent *entry;
     struct stat entry_stat;
     char full_path[512];
     int file_count = 0;
 
     while ((entry = readdir(dir)) != NULL) {
         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
             continue;
         }
 
         #pragma GCC diagnostic push
         #pragma GCC diagnostic ignored "-Wformat-truncation"
         snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
         #pragma GCC diagnostic pop
 
         if (stat(full_path, &entry_stat) == -1) {
             continue;
         }
 
         if (!S_ISDIR(entry_stat.st_mode)) {
             file_count++;
         }
     }
 
     closedir(dir);
     return file_count;
 }
 
 /**
  * @brief Checks if a directory exists on the SD card
  * @param path Relative path from SD mount point
  * @return true if directory exists, false otherwise
  * 
  * @example bool exists = sd_card_dir_exists("recordings/2023");
  */
 bool sd_card_dir_exists(const char *path) {
     char full_path[256];
     snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, path);
 
     if (!sd_card_mounted) {
         ESP_LOGE(TAG, "SD card not mounted");
         return false;
     }
 
     struct stat st;
     if (stat(full_path, &st) != 0) {
         ESP_LOGD(TAG, "Path %s not accessible: %s", full_path, strerror(errno));
         return false;
     }
 
     if (!S_ISDIR(st.st_mode)) {
         ESP_LOGD(TAG, "Path %s exists but is not a directory", full_path);
         return false;
     }
 
     ESP_LOGD(TAG, "Directory %s exists", full_path);
     return true;
 }