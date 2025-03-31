/*

    Steps to Accomplish Task

    1) Create an SPIFFS Partition and hardcode the initial data with frequency 0

    2) Create a tab where it lists the file system of the memory card

    3) Give a provision to delete the file from the file system    
    
*/

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "station_mode_wifi.h"
#include "https_server_hosting.h"
#include "i2s_recorder.h"

static const char *TAG = "sound_classification_esp32";

void app_main(void){

    // Start the WIFI Operation First
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();


    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/web",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&conf);

    start_https_server();
}



