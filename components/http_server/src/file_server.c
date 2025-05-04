/*
* SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Unlicense OR CC0-1.0
*/

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "i2s_recorder_main.h"
#include "file_server.h"
#include "esp_vfs_fat.h"
#include "file_operations.h"
#include "esp_timer.h"

static const char *TAG = "file_server";

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

static void httpd_close_func(httpd_handle_t hd, int sockfd)
{
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    ESP_LOGD(TAG, "Forcefully closed socket %d", sockfd);
}

// Timer callback at file scope
static void recording_timer_callback(void* arg) {
    bool* recording_flag = (bool*)arg;
    *recording_flag = false;
}

// Update the start_recording_handler function
esp_err_t start_recording_handler(httpd_req_t *req) {
    static bool is_recording = false;
    
    if (is_recording) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Recording already in progress");
        return ESP_FAIL;
    }

    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Query required");
        return ESP_FAIL;
    }

    char category[32];
    if (httpd_query_key_value(query, "category", category, sizeof(category)) != ESP_OK) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Category missing");
        return ESP_FAIL;
    }

    char *task_category = strdup(category);
    if (!task_category) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    is_recording = true;
    xTaskCreate(
        (TaskFunction_t)start_recording,
        "rec_task",
        4096,
        (void*)task_category,
        5,
        NULL
    );

    // Timer setup
    const esp_timer_create_args_t timer_args = {
        .callback = &recording_timer_callback,
        .arg = &is_recording,
        .name = "recording_timer"
    };
    
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, CONFIG_EXAMPLE_REC_TIME * 1000000 + 1000000));

    httpd_resp_send(req, "Recording started", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t list_files_handler(httpd_req_t *req) {
    char path[256];
    snprintf(path, sizeof(path), "%s", SD_MOUNT_POINT); // Start with mount point

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char* query = malloc(query_len + 1);
        if (!query) {
            return ESP_ERR_NO_MEM;
        }

        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            char param[128];
            if (httpd_query_key_value(query, "path", param, sizeof(param)) == ESP_OK) {
                // Sanitize and append path
                if (param[0] == '/') {
                    snprintf(path, sizeof(path), "%s%s", SD_MOUNT_POINT, param);
                } else {
                    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, param);
                }
            }
        }
        free(query);
    }

    // Security checks
    if (strstr(path, "../") || strstr(path, "..\\")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    // Ensure proper path termination
    if (strlen(path) > 1 && path[strlen(path)-1] == '/') {
        path[strlen(path)-1] = '\0';
    }

    // Verify directory exists
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory not found");
        return ESP_FAIL;
    }

    // Generate and send response
    char *json_response = list_files_json(path);
    if (!json_response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate listing");
        return ESP_FAIL;
    }

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

esp_err_t script_js_handler(httpd_req_t *req)
{
    extern const unsigned char script_js_start[] asm("_binary_script_js_start");
    extern const unsigned char script_js_end[] asm("_binary_script_js_end");
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)script_js_start, script_js_end - script_js_start);
}

esp_err_t style_css_get_handler(httpd_req_t *req)
{
    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[] asm("_binary_style_css_end");
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}

esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);
}

esp_err_t send_index_html(httpd_req_t *req)
{
    extern const unsigned char index_start[] asm("_binary_index_html_start");
    extern const unsigned char index_end[] asm("_binary_index_html_end");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, (const char *)index_start, index_end - index_start);
    
    if (ret == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    return ret;
}

esp_err_t delete_post_handler(httpd_req_t *req)
{
    struct file_server_data *server_data = req->user_ctx;
    char filepath[FILE_PATH_MAX];
    
    const char *filename = get_path_from_uri(filepath, server_data->base_path,
                                           req->uri + sizeof("/delete") - 1, sizeof(filepath));
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* File deletion implementation here */
    delete_path(filename);
    /* ... */

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "File deleted successfully");
}

esp_err_t download_get_handler(httpd_req_t *req)
{
    struct file_server_data *server_data = req->user_ctx;
    char filepath[FILE_PATH_MAX];
    const char *filename = get_path_from_uri(filepath, server_data->base_path, req->uri, sizeof(filepath));
    
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    // Special handling for root path
    if (strcmp(filename, "/") == 0) {
        return send_index_html(req);
    }

    // Check for other static files
    if (strcmp(filename, "/index.html") == 0) return send_index_html(req);
    if (strcmp(filename, "/favicon.ico") == 0) return favicon_get_handler(req);
    if (strcmp(filename, "/style.css") == 0) return style_css_get_handler(req);
    if (strcmp(filename, "/script.js") == 0) return script_js_handler(req);

    struct stat file_stat;
    if (stat(filepath, &file_stat) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filename);
    httpd_resp_set_hdr(req, "Connection", "close");

    char *chunk = server_data->scratch;
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t start_file_server(const char *base_path)
{
    static struct file_server_data *server_data = NULL;
    if (server_data) return ESP_ERR_INVALID_STATE;

    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) return ESP_ERR_NO_MEM;
    strlcpy(server_data->base_path, base_path, sizeof(server_data->base_path));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.keep_alive_enable = false;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20;
    config.close_fn = httpd_close_func;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        free(server_data);
        return ESP_FAIL;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = send_index_html, .user_ctx = server_data},
        {.uri = "/index.html", .method = HTTP_GET, .handler = send_index_html, .user_ctx = server_data},
        {.uri = "/style.css", .method = HTTP_GET, .handler = style_css_get_handler, .user_ctx = server_data},
        {.uri = "/script.js", .method = HTTP_GET, .handler = script_js_handler, .user_ctx = server_data},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = server_data},
        {.uri = "/delete/*", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = server_data},
        {.uri = "/start_recording*", .method = HTTP_GET, .handler = start_recording_handler, .user_ctx = server_data},
        {.uri = "/list_files", .method = HTTP_GET, .handler = list_files_handler, .user_ctx = NULL},
        {.uri = "/delete_file", .method = HTTP_GET, .handler = delete_file_handler, .user_ctx = NULL},
        {.uri = "/download_file", .method = HTTP_GET, .handler = download_file_handler, .user_ctx = NULL},
        {.uri = "/*", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = server_data},
    };

    for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }

    return ESP_OK;
}