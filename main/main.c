/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "file_server.h"
#include "model_predictor.h"
#include "soft_access_point.h"

/* This example demonstrates how to create file server
 * using esp_http_server. This file has only startup code.
 * Look in file_server.c for the implementation.
 */

static const char *TAG = "example";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting example");
    
    // 1. Initialize NVS (required for WiFi)
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // 2. Create event loop FIRST (before any WiFi operations)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 3. Initialize WiFi in AP mode
    soft_access_create(); // Now properly modified
    
    // 4. Mount storage
    const char* base_path = "/data";
    ESP_ERROR_CHECK(mount_storage(base_path));
    
    // 5. Start web server
    ESP_ERROR_CHECK(start_file_server(base_path));
    ESP_LOGI(TAG, "File server started at http://192.168.4.1");
    
    // Your prediction code (if needed)
    const float input_data[15] = {13.6, 18.2, 11.3, 14.5, 17.8, 19.0, 21.3, 23.4, 21.7, 16.5, 19.7, 28.3, 24.2, 20.7, 9.3};
    ESP_LOGI(TAG, "The value predicted is: %d", predict_class(input_data));
}
