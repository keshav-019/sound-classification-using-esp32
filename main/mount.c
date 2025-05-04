/**
 * @file storage_mount.c
 * @brief Storage mounting implementation for SD card and SPIFFS
 * 
 * This file provides functionality to:
 * - Mount SD cards using either SDMMC or SPI interfaces
 * - Mount SPIFFS partitions for internal flash storage
 * - Handle filesystem formatting when needed
 * - Provide storage abstraction for the file server
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#include "sdmmc_cmd.h"
#include "file_server.h"

static const char *TAG = "storage_mount";  ///< Logging tag for storage operations

#ifdef CONFIG_EXAMPLE_MOUNT_SD_CARD

/**
* @brief Mounts an SD card storage device
* @param base_path Path where the filesystem should be mounted
* @return ESP_OK on success, error code on failure
* 
* This function handles SD card initialization with two possible interfaces:
* 1. SDMMC (4-bit native interface) - Higher performance
* 2. SPI (compatible mode) - Works with more hardware
* 
* Configuration is controlled by these Kconfig options:
* - CONFIG_EXAMPLE_USE_SDMMC_HOST: Selects SDMMC vs SPI interface
* - CONFIG_EXAMPLE_FORMAT_IF_MOUNT_SDCARD_FAILED: Auto-format on failure
* 
* The mounting process follows these steps:
* 1. Configure mount parameters (formatting, max files, etc.)
* 2. Initialize the selected host interface
* 3. Mount the FAT filesystem
* 4. Print card information if successful
*/
esp_err_t mount_storage(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing SD card");

    // Step 1: Configure filesystem mount parameters
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_SDCARD_FAILED
        .format_if_mount_failed = true,  // Auto-format if mount fails
#else
        .format_if_mount_failed = false, // Fail if filesystem appears corrupt
#endif
        .max_files = 5,                 // Maximum open files
        .allocation_unit_size = 16 * 1024 // Allocation unit size
    };

    esp_err_t ret;
    sdmmc_card_t* card;

#ifdef CONFIG_EXAMPLE_USE_SDMMC_HOST
    // SDMMC Host Configuration (4-bit native interface)
    ESP_LOGI(TAG, "Using SDMMC peripheral");
    
    // Step 2a: Initialize SDMMC host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    
    // Step 3a: Configure SDMMC slot
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  // 4-bit bus mode

#ifdef SOC_SDMMC_USE_GPIO_MATRIX
    // Configure GPIO pins for SDMMC interface
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
    slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
#endif // SOC_SDMMC_USE_GPIO_MATRIX

    // Enable weak internal pullups (external pullups recommended)
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Step 4a: Mount SDMMC filesystem
    ret = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot_config, &mount_config, &card);

#else // SPI Host Configuration

    // Step 2b: Initialize SPI host
    ESP_LOGI(TAG, "Using SPI peripheral");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    // Step 3b: Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_EXAMPLE_PIN_MOSI,
        .miso_io_num = CONFIG_EXAMPLE_PIN_MISO,
        .sclk_io_num = CONFIG_EXAMPLE_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus");
        return ret;
    }

    // Step 4b: Configure SPI device
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_EXAMPLE_PIN_CS;
    slot_config.host_id = host.slot;
    
    // Step 5b: Mount SPI filesystem
    ret = esp_vfs_fat_sdspi_mount(base_path, &host, &slot_config, &mount_config, &card);

#endif // Host selection

    // Error handling
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                    "Enable EXAMPLE_FORMAT_IF_MOUNT_FAILED to auto-format");
        } else {
            ESP_LOGE(TAG, "Failed to initialize card (%s). "
                    "Check pull-up resistors on SD card lines", esp_err_to_name(ret));
        }
        return ret;
    }

    // Success case
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

#else // SPIFFS Implementation
 
/**
* @brief Mounts a SPIFFS partition for internal flash storage
* @param base_path Path where the filesystem should be mounted
* @return ESP_OK on success, error code on failure
* 
* This function handles SPIFFS initialization with these features:
* - Automatic formatting if mount fails (configurable)
* - Partition size reporting
* - Filesystem validation
* 
* The mounting process follows these steps:
* 1. Configure SPIFFS mount parameters
* 2. Register SPIFFS with VFS
* 3. Verify partition information
*/
esp_err_t mount_storage(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    // Step 1: Configure SPIFFS mount parameters
    esp_vfs_spiffs_conf_t conf = {
        .base_path = base_path,
        .partition_label = NULL,  // Use first SPIFFS partition found
        .max_files = 5,          // Maximum open files
        .format_if_mount_failed = true  // Auto-format if mount fails
    };

    // Step 2: Register SPIFFS with VFS
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    // Step 3: Verify partition information
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

#endif // Storage selection