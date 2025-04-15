#include "file_server.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_vfs.h"

static const char *TAG = "file_handlers";

static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) pathlen = MIN(pathlen, quest - uri);
    const char *hash = strchr(uri, '#');
    if (hash) pathlen = MIN(pathlen, hash - uri);

    if (base_pathlen + pathlen + 1 > destsize) return NULL;

    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    return dest + base_pathlen;
}

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (strcasecmp(&filename[strlen(filename) - 4], ".pdf") == 0) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (strcasecmp(&filename[strlen(filename) - 5], ".html") == 0) {
        return httpd_resp_set_type(req, "text/html");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".jpg") == 0 ||
               strcasecmp(&filename[strlen(filename) - 5], ".jpeg") == 0) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".png") == 0) {
        return httpd_resp_set_type(req, "image/png");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".ico") == 0) {
        return httpd_resp_set_type(req, "image/x-icon");
    } else if (strcasecmp(&filename[strlen(filename) - 3], ".js") == 0) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".css") == 0) {
        return httpd_resp_set_type(req, "text/css");
    }
    return httpd_resp_set_type(req, "text/plain");
}

esp_err_t list_files_handler(httpd_req_t *req) {
    mount_sdcard();

    struct stat st;
    if (stat(SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Directory %s does not exist or is not accessible", SD_MOUNT_POINT);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card not mounted");
        return ESP_FAIL;
    }

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s: %s", SD_MOUNT_POINT, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }

    char *json_response = malloc(LIST_BUFFER_SIZE);
    if (!json_response) {
        closedir(dir);
        ESP_LOGE(TAG, "Failed to allocate memory for JSON response");
        return ESP_ERR_NO_MEM;
    }
    
    strcpy(json_response, "{\"files\":[");

    int file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < MAX_FILES) {
        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            char full_path[MAX_PATH_LENGTH];
            snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, entry->d_name);

            if (stat(full_path, &st) != 0) {
                continue;
            }
            
            if (file_count > 0) {
                strcat(json_response, ",");
            }
            
            int written = snprintf(json_response + strlen(json_response), 
                                 LIST_BUFFER_SIZE - strlen(json_response),
                                 "{\"name\":\"%s\",\"is_dir\":%d,\"size\":%d}",
                                 entry->d_name,
                                 entry->d_type == DT_DIR,
                                 (int)st.st_size);
            
            if (written < 0 || written >= (LIST_BUFFER_SIZE - strlen(json_response))) {
                break;
            }
            
            file_count++;
        }
    }
    closedir(dir);
    
    strcat(json_response, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_response, strlen(json_response));
    free(json_response);
    
    return ret;
}

esp_err_t delete_file_handler(httpd_req_t *req) {
    char filepath[MAX_PATH_LENGTH];
    char decoded_path[MAX_PATH_LENGTH];
    
    if (httpd_req_get_url_query_str(req, filepath, sizeof(filepath)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename required");
        return ESP_FAIL;
    }
    
    char full_path[MAX_PATH_LENGTH];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, decoded_path) >= sizeof(full_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
        return ESP_FAIL;
    }
    
    if (unlink(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }
    
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

esp_err_t download_file_handler(httpd_req_t *req) {
    char filepath[MAX_PATH_LENGTH];
    char decoded_path[MAX_PATH_LENGTH];
    
    if (httpd_req_get_url_query_str(req, filepath, sizeof(filepath)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename required");
        return ESP_FAIL;
    }
    
    char full_path[MAX_PATH_LENGTH];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, decoded_path) >= sizeof(full_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
        return ESP_FAIL;
    }
    
    FILE *file = fopen(full_path, "rb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    struct stat file_stat;
    if (stat(full_path, &file_stat) == -1) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get file stats");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    
    char *chunk = malloc(1024);
    if (!chunk) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 1024, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            break;
        }
    }
    
    free(chunk);
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}