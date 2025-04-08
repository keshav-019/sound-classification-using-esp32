/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* I2S Digital Microphone Recording Example */
#include "i2s_recorder_main.h"

static const char *TAG = "pdm_rec_example";

#define CONFIG_EXAMPLE_BIT_SAMPLE       16
#define SPI_DMA_CHAN                    SPI_DMA_CH_AUTO
#define NUM_CHANNELS                    (1) // For mono recording only!
#define SD_MOUNT_POINT                  "/sdcard"
#define SAMPLE_SIZE                     (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE                       (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS
#define CONFIG_EXAMPLE_REC_TIME         15

// When testing SD and SPI modes, keep in mind that once the card has been
// initialized in SPI mode, it can not be reinitialized in SD mode without
// toggling power to the card.
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
i2s_chan_handle_t rx_handle = NULL;

static int16_t i2s_readraw_buff[SAMPLE_SIZE];
size_t bytes_read;
const int WAVE_HEADER_SIZE = 44;

void mount_sdcard(void)
{
    esp_err_t ret;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };
    ESP_LOGI(TAG, "Initializing SD card");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_EXAMPLE_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_EXAMPLE_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_EXAMPLE_SPI_CS_GPIO;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
}

void record_wav(uint32_t rec_time, const char* category_name)
{
    // Use POSIX and C standard library functions to work with files.
    int flash_wr_size = 0;
    ESP_LOGI(TAG, "Opening file");

    uint32_t flash_rec_time = BYTE_RATE * rec_time;
    const wav_header_t wav_header =
        WAV_HEADER_PCM_DEFAULT(flash_rec_time, 16, CONFIG_EXAMPLE_SAMPLE_RATE, 1);

    // Create full file path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s/record.wav", SD_MOUNT_POINT, category_name);

    // Create directory if it doesn't exist
    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", SD_MOUNT_POINT, category_name);
    mkdir(dirpath, 0777);  // Create category directory with full permissions

    // First check if file exists before creating a new file.
    struct stat st;
    if (stat(filepath, &st) == 0) {
        // Delete it if it exists
        unlink(filepath);
    }

    // Create new WAV file
    FILE *f = fopen(filepath, "wb");  // Use "wb" for binary write mode
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return;
    }

    // Write the header to the WAV file
    if (fwrite(&wav_header, sizeof(wav_header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(f);
        return;
    }

    // Start recording
    while (flash_wr_size < flash_rec_time) {
        // Read the RAW samples from the microphone
        if (i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 1000) == ESP_OK) {
            ESP_LOGV(TAG, "[0] %d [1] %d [2] %d [3] %d", 
                   i2s_readraw_buff[0], i2s_readraw_buff[1], 
                   i2s_readraw_buff[2], i2s_readraw_buff[3]);
            
            // Write the samples to the WAV file
            if (fwrite(i2s_readraw_buff, bytes_read, 1, f) != 1) {
                ESP_LOGE(TAG, "Write failed at %d bytes", flash_wr_size);
                break;
            }
            flash_wr_size += bytes_read;
        } else {
            ESP_LOGW(TAG, "Read Failed!");
        }
    }

    ESP_LOGI(TAG, "Recording done! Wrote %d bytes", flash_wr_size);
    fclose(f);
    ESP_LOGI(TAG, "File written: %s", filepath);

    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    ESP_LOGI(TAG, "Card unmounted");
    spi_bus_free(host.slot);  // Deinitialize the bus
}

void init_microphone(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(CONFIG_EXAMPLE_SAMPLE_RATE),
        /* The default mono slot is the left slot (whose 'select pin' of the PDM microphone is pulled down) */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = CONFIG_EXAMPLE_I2S_CLK_GPIO,
            .din = CONFIG_EXAMPLE_I2S_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

void start_recording(const char* category_name) {
    printf("Starting recording for category: %s\n", category_name);
    mount_sdcard();
    init_microphone();
    record_wav(CONFIG_EXAMPLE_REC_TIME, category_name);
    
    // Cleanup
    ESP_ERROR_CHECK(i2s_channel_disable(rx_handle));
    ESP_ERROR_CHECK(i2s_del_channel(rx_handle));
    
    // Free the category_name string (allocated via strdup in the handler)
    free((void*)category_name);
    
    // Delete this task when done
    vTaskDelete(NULL);
}
