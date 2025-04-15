/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example, common declarations

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#ifndef FILE_SERVER_H
#define FILE_SERVER_H

#include "esp_http_server.h"

#define FILE_PATH_MAX (800 + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define MAX_FILE_SIZE (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"
#define SCRATCH_BUFSIZE 8192
#define SD_MOUNT_POINT "/sdcard"
#define MAX_FILES 50
#define LIST_BUFFER_SIZE 4096
#define MAX_PATH_LENGTH 512

struct file_server_data {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
};

typedef struct {
    char name[256];
    int is_dir;
    size_t size;
} file_info_t;

esp_err_t download_get_handler(httpd_req_t *req);

static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize);

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename);

esp_err_t list_files_handler(httpd_req_t *req);

esp_err_t delete_file_handler(httpd_req_t *req);

esp_err_t download_file_handler(httpd_req_t *req);

esp_err_t start_recording_handler(httpd_req_t *req);

static void httpd_close_func(httpd_handle_t hd, int sockfd);

esp_err_t script_js_handler(httpd_req_t *req);

esp_err_t style_css_get_handler(httpd_req_t *req);

esp_err_t favicon_get_handler(httpd_req_t *req);

esp_err_t send_index_html(httpd_req_t *req);

esp_err_t upload_post_handler(httpd_req_t *req);

esp_err_t delete_post_handler(httpd_req_t *req)

esp_err_t start_file_server(const char *base_path);

#endif // FILE_SERVER_H
