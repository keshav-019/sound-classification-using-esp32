#include "file_operations.h"
#include "i2s_recorder_main.h"

#include <ctype.h>  // Added for isprint()
#include <time.h>   // Added for localtime() and strftime()

static const char *TAG = "pdm_rec_example";

void list_files(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }

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

// Helper function to escape JSON strings
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

// Safely append to JSON buffer with reallocation if needed
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

char* list_files_json(const char *path) {
    // Ensure path is valid
    if (!path || path[0] == '\0') {
        return NULL;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return NULL;
    }

    // Initial buffer with extra space
    size_t buf_size = 1024;
    char *json_buffer = malloc(buf_size);
    if (!json_buffer) {
        closedir(dir);
        return NULL;
    }

    // Start JSON
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

        // Escape strings
        char escaped_name[1024];
        char escaped_full_path[2048];
        json_escape_string(escaped_name, sizeof(escaped_name), entry->d_name);
        json_escape_string(escaped_full_path, sizeof(escaped_full_path), full_path);

        // Format time
        char time_buf[64];
        struct tm *tm_info = localtime(&entry_stat.st_mtime);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

        // Check if we need to grow buffer
        size_t needed = offset + 512; // Conservative estimate
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

        // Append entry
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

    // Close JSON
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

// [Rest of your file operations implementations remain exactly the same]

void create_directory(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
    int ret = mkdir(path, 0777);
    if (ret == -1) {
        ESP_LOGE("MKDIR", "Failed to create directory %s: %s", path, strerror(errno));
    } else {
        ESP_LOGI("MKDIR", "Created directory: %s", path);
    }
}

void delete_path(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
    struct stat path_stat;
    if (stat(path, &path_stat)) {
        ESP_LOGE("DELETE", "Path doesn't exist: %s", path);
        return;
    }

    if (S_ISDIR(path_stat.st_mode)) {
        // Recursive directory deletion
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

void copy_file(const char *src_path, const char *dest_path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
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

void move_file(const char *old_path, const char *new_path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
    if (rename(old_path, new_path)) {
        ESP_LOGE("MOVE", "Failed to move %s to %s: %s", old_path, new_path, strerror(errno));
    } else {
        ESP_LOGI("MOVE", "Moved %s to %s", old_path, new_path);
    }
}

size_t get_file_size(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
    struct stat st;
    if (stat(path, &st)) {
        ESP_LOGE("SIZE", "Failed to get size of %s: %s", path, strerror(errno));
        return 0;
    }
    return st.st_size;
}

char* read_file_content(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }
    
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


int count_files_in_directory(const char *path) {
    mount_sdcard();
    // if(mount_sdcard() != ESP_OK){
    //     ESP_LOGE(TAG, "Could not mount SD Card. Either SD Card is already initialized or SD Card is missing");
    // }

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
        // Skip "." and ".." entries
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

        // Count only regular files (not directories)
        if (!S_ISDIR(entry_stat.st_mode)) {
            file_count++;
        }
    }

    closedir(dir);
    return file_count;
}




/**
 * @brief Check if a directory exists on the SD card
 * @param path The directory path to check (relative to SD mount point)
 * @return true if directory exists, false otherwise
 */
 bool sd_card_dir_exists(const char *path) {
    // Construct full path
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, path);

    // Check if SD card is mounted first
    if (!sd_card_mounted) {
        ESP_LOGE(TAG, "SD card not mounted");
        return false;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        // Error accessing path
        ESP_LOGD(TAG, "Path %s not accessible: %s", full_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        // Path exists but isn't a directory
        ESP_LOGD(TAG, "Path %s exists but is not a directory", full_path);
        return false;
    }

    ESP_LOGD(TAG, "Directory %s exists", full_path);
    return true;
}