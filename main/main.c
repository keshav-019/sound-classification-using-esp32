/**
* @file main.c
* @brief Main application file for HTTP File Server with Audio Recording
* 
* This file implements the main application logic that:
* - Initializes the system components
* - Starts the WiFi Access Point
* - Mounts the storage system
* - Launches the HTTP file server
* - Provides audio recording functionality
*/

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "i2s_recorder_main.h"
#include "file_server.h"
#include "model_predictor.h"
#include "soft_access_point.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "format_wav.h"
#include <stdlib.h>
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"
#include "soc/uart_struct.h"
#include "esp_dsp.h"
#include "file_operations.h"

static const char *TAG = "main_function";  ///< Logging tag for main application

/**
* @brief Main application entry point
* 
* Initializes all system components in the following order:
* 1. Non-Volatile Storage (NVS) - Required for WiFi configuration
* 2. Event loop - Needed before any WiFi operations
* 3. WiFi Access Point - Creates the soft AP for client connections
* 4. Storage system - Mounts the SD card/filesystem
* 5. HTTP File Server - Starts the web server for file management
* 
* The initialization sequence is critical - components must be started
* in the correct order to ensure proper operation.
* 
* @note This function never returns as it runs on the FreeRTOS scheduler
*/
void app_main(void)
{
    ESP_LOGI(TAG, "Starting application initialization");
    
    /**************************************************************************
    * Step 1: Initialize Non-Volatile Storage (NVS)
    * 
    * Required for WiFi configuration storage. This allows the system to
    * remember WiFi settings across reboots.
    *************************************************************************/
    ESP_ERROR_CHECK(nvs_flash_init());
    
    /**************************************************************************
    * Step 2: Create Event Loop
    * 
    * Must be created before any WiFi operations. Handles system events:
    * - WiFi connection events
    * - IP address assignment
    * - HTTP server events
    *************************************************************************/
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /**************************************************************************
    * Step 3: Initialize WiFi in Access Point Mode
    * 
    * Creates a soft AP with the following configuration:
    * - SSID: Defined in EXAMPLE_ESP_WIFI_AP_SSID
    * - Password: Defined in EXAMPLE_ESP_WIFI_AP_PASSWD
    * - IP Address: 192.168.4.1 (hardcoded in soft_access_point.c)
    *************************************************************************/
    soft_access_create();
    
    /**************************************************************************
    * Step 4: Initialize and Mount Storage
    * 
    * Lists existing files (debug) and mounts the storage system:
    * - For SD card: Initializes SPI interface and mounts FAT filesystem
    * - For SPIFFS: Mounts the internal flash filesystem
    *************************************************************************/
    // Debug: List existing files on SD card
    list_files("/sdcard");
    
    // Mount the storage system (either SD card or SPIFFS)
    const char* base_path = "/data";
    ESP_ERROR_CHECK(mount_storage(base_path));
    
    /**************************************************************************
    * Step 5: Start HTTP File Server
    * 
    * Launches the web server with the following capabilities:
    * - File upload/download
    * - Directory listing
    * - Audio recording control
    * - File management (delete/rename)
    * 
    * Accessible at http://192.168.4.1
    *************************************************************************/
    ESP_ERROR_CHECK(start_file_server(base_path));
    ESP_LOGI(TAG, "File server started at http://192.168.4.1");

    /**************************************************************************
    * Optional: Model Prediction Example (commented out)
    * 
    * Demonstrates how to use the audio classification model:
    * - Takes MFCC features as input
    * - Returns predicted class index
    * 
    * Uncomment to test model functionality
    *************************************************************************/
    /*
    const float input_data[15] = {13.6, 18.2, 11.3, 14.5, 17.8, 19.0, 
                                21.3, 23.4, 21.7, 16.5, 19.7, 28.3, 
                                24.2, 20.7, 9.3};
    ESP_LOGI(TAG, "The value predicted is: %d", predict_class(input_data));
    */
}