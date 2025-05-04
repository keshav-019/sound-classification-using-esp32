/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "i2s_recorder_main.h"
#include <dirent.h>
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
#include "model_predictor.h"
#include "file_operations.h"

// custom library addition
#include <stdlib.h>
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"
#include "soc/uart_struct.h"
#include "esp_dsp.h"

static const char *TAG = "pdm_rec_example";

#define CONFIG_EXAMPLE_BIT_SAMPLE       16
#define SPI_DMA_CHAN                    SPI_DMA_CH_AUTO
#define NUM_CHANNELS                    (1)
#define SD_MOUNT_POINT                  "/sdcard"
#define SAMPLE_SIZE                     (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE                       (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS
#define CONFIG_EXAMPLE_REC_TIME         5


// Constant declaration
#define frame_size 1024
#define stride 128
#define sampling_rate 16000
#define record_duration 5 // in seconds
#define MAX_BUFFER_SIZE 1024
#define num_mfcc 15
// #define WAVE_HEADER_SIZE 44 // in bytes

#define SPI_DMA_CHAN        SPI_DMA_CH_AUTO
#define NUM_CHANNELS        (1) // For mono recording only!
#define SD_MOUNT_POINT      "/sdcard"
#define SAMPLE_SIZE         (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE           (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS

int N = frame_size;
// Input test array
__attribute__((aligned(16)))
float x1[frame_size];
__attribute__((aligned(16)))
float x2[frame_size];
// Window coefficients
__attribute__((aligned(16)))
float wind[frame_size];
// Pointers to result arrays
float *y1_cf = &x1[0];
float *y2_cf = &x2[0];

// diff of y1 and y2
__attribute__((aligned(16)))
float diff_y[frame_size / 2];

#define frame_size 1024 // FFT length
#define NUM_MEL_BINS 23 // Number of Mel filter banks (typically 20-40)
#define NUM_MFCC_COEFFS 13 // Number of MFCCs we want to extract

// Constants for Mel-scale
#define SAMPLING_RATE 16000 // Example: 16kHz sample rate
#define FFT_BIN_SIZE (SAMPLING_RATE / 2) / (frame_size / 2)

float mel_filter_bank[NUM_MEL_BINS][frame_size / 2]; // Mel filter bank
float mel_spectrum[NUM_MEL_BINS]; // Mel-spectrum
float log_mel_spectrum[NUM_MEL_BINS]; // Log of Mel-spectrum
float dct_matrix[NUM_MEL_BINS][NUM_MFCC_COEFFS]; // DCT matrix
float mfcc[NUM_MFCC_COEFFS]; // MFCC coefficients

sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
i2s_chan_handle_t rx_handle = NULL;
bool sd_card_mounted;

static int16_t i2s_readraw_buff[SAMPLE_SIZE];
size_t bytes_read;
const int WAVE_HEADER_SIZE = 44;

// Global map to keep track of recording numbers per category
static int category_counter = 0;

// Function to generate the Mel filter bank
void generate_mel_filter_bank() {
    float f_min = 0; // Minimum frequency
    float f_max = SAMPLING_RATE / 2; // Maximum frequency (Nyquist)

    // Convert to Mel scale
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        float f_mel = 2595 * log10(1 + (f_min + (f_max - f_min) * m / (NUM_MEL_BINS - 1)) / 700);
        int lower_bin = (int)((f_mel - 2595 * log10(1 + f_min / 700)) / FFT_BIN_SIZE);
        int upper_bin = (int)((f_mel - 2595 * log10(1 + f_max / 700)) / FFT_BIN_SIZE);
        for (int i = lower_bin; i <= upper_bin; i++) {
            mel_filter_bank[m][i] = 1.0;
        }
    }
}

// Function to compute the Mel spectrum from the FFT output
void compute_mel_spectrum(float* fft_result) {
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        mel_spectrum[m] = 0.0;
        for (int f = 0; f < frame_size / 2; f++) {
            mel_spectrum[m] += fft_result[f] * mel_filter_bank[m][f];
        }
    }
}

// Function to apply log to Mel spectrum
void apply_log_to_mel_spectrum() {
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        // Check if the Mel spectrum value is too small and add a small constant to avoid log(0)
        log_mel_spectrum[m] = log10(fmax(mel_spectrum[m], 1e-10)); // Use fmax to ensure it's never zero
    }
}

// Function to apply DCT to the log Mel spectrum
void apply_dct() {
    for (int c = 0; c < NUM_MFCC_COEFFS; c++) {
        mfcc[c] = 0.0;
        for (int m = 0; m < NUM_MEL_BINS; m++) {
                // Use double precision to increase accuracy
            double temp = log_mel_spectrum[m] * cos(M_PI * c * (2 * m + 1) / (2.0 * NUM_MEL_BINS));
            mfcc[c] += temp;
        }
        // Apply a scaling factor to the first coefficient
        if (c == 0) {
            mfcc[c] /= sqrt(2.0);
        }
    }
}

// Arrays for number words
static const char *units[] = {"", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
static const char *teens[] = {"ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
static const char *tens[] = {"", "ten", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
static const char *scales[] = {"", "thousand", "million", "billion"};

// Helper function to convert chunks of 3 digits
static void convert_chunk(unsigned int chunk, char *buffer, int scale) {
    if (chunk == 0) return;
    
    char temp[256] = {0};
    int pos = 0;
    
    // Hundreds place
    if (chunk >= 100) {
        int h = chunk / 100;
        pos += sprintf(temp + pos, "%s_hundred", units[h]);
        chunk %= 100;
    }
    
    // Tens and units
    if (chunk >= 20) {
        int t = chunk / 10;
        pos += sprintf(temp + pos, "%s%s", pos ? "_" : "", tens[t]);
        chunk %= 10;
        if (chunk > 0) {
            pos += sprintf(temp + pos, "_%s", units[chunk]);
        }
    } else if (chunk >= 10) {
        pos += sprintf(temp + pos, "%s%s", pos ? "_" : "", teens[chunk - 10]);
    } else if (chunk > 0) {
        pos += sprintf(temp + pos, "%s%s", pos ? "_" : "", units[chunk]);
    }
    
    // Add scale if needed
    if (scale > 0) {
        sprintf(temp + pos, "_%s", scales[scale]);
    }
    
    // Append to main buffer
    if (buffer[0] != '\0') {
        strcat(buffer, "_");
    }
    strcat(buffer, temp);
}

// Main conversion function
char* number_to_words(int num) {
    if (num == 0) return strdup("zero");
    
    char *result = calloc(256, sizeof(char));
    if (!result) return NULL;
    
    int is_negative = 0;
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    
    int scale = 0;
    unsigned int chunks[4] = {0};
    
    // Split into chunks of 3 digits
    while (num > 0) {
        chunks[scale++] = num % 1000;
        num /= 1000;
    }
    
    // Process each chunk
    for (int i = scale - 1; i >= 0; i--) {
        if (chunks[i] != 0) {
            convert_chunk(chunks[i], result, i);
        }
    }
    
    if (is_negative) {
        char temp[256];
        sprintf(temp, "negative_%s", result);
        strcpy(result, temp);
    }
    
    return result;
}

// Function to get the next available recording number for a category
static int get_next_recording_number(const char* category_name) {
    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", SD_MOUNT_POINT, category_name);
    
    DIR *dir = opendir(dirpath);
    if (!dir) {
        return 1; // First recording for this category
    }

    int max_num = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            int num;
            if (sscanf(entry->d_name, "record_%d.wav", &num) == 1) {
                if (num > max_num) {
                    max_num = num;
                }
            }
        }
    }
    closedir(dir);
    
    return max_num + 1;
}

void mount_sdcard(void) {
    esp_err_t ret;
    
    // Unmount first if already mounted
    if (sd_card_mounted) {
        unmount_sdcard();
    }

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
        ESP_LOGE(TAG, "Failed to initialize bus: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_EXAMPLE_SPI_CS_GPIO;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return;
    }

    sdmmc_card_print_info(stdout, card);
    sd_card_mounted = true;
}

// Function to properly unmount SD card
void unmount_sdcard(void) {
    if (!sd_card_mounted) {
        ESP_LOGW(TAG, "SD card not mounted, skipping unmount");
        return;
    }

    ESP_LOGI(TAG, "Unmounting SD card");
    esp_err_t ret = ESP_OK;

    // 1. Unmount the filesystem
    ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount filesystem: %s", esp_err_to_name(ret));
    }

    // 2. Free the SPI bus
    ret = spi_bus_free(host.slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to free SPI bus: %s", esp_err_to_name(ret));
    }

    // 3. Clear card pointer and mounted flag
    card = NULL;
    sd_card_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted successfully");
    return;
}

void record_wav(uint32_t rec_time, const char* category_name) {
    // if (mount_sdcard() != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to mount SD card");
    //     return;
    // }

    mount_sdcard();

    int flash_wr_size = 0;
    ESP_LOGI(TAG, "Opening file");

    uint32_t flash_rec_time = BYTE_RATE * rec_time;
    const wav_header_t wav_header =
        WAV_HEADER_PCM_DEFAULT(flash_rec_time, 16, CONFIG_EXAMPLE_SAMPLE_RATE, 1);

    // Create directory if it doesn't exist
    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", SD_MOUNT_POINT, category_name);
    // mkdir(dirpath, 0777);
    if(!sd_card_dir_exists(dirpath)){
        mkdir(dirpath, 0777);
    }

    // Get next available recording number
    int recording_num = count_files_in_directory(dirpath) + 1;

    ESP_LOGI("TAG", "The recording number right now is: %d", recording_num);
    
    // Create unique filename
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s/rec_%d.wav", SD_MOUNT_POINT, category_name, recording_num);

    // Create new WAV file
    FILE *f = fopen(filepath, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        unmount_sdcard();
        return;
    }

    // Write the header to the WAV file
    if (fwrite(&wav_header, sizeof(wav_header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(f);
        unmount_sdcard();
        return;
    }

    // Start recording
    while (flash_wr_size < flash_rec_time) {
        if (i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 1000) == ESP_OK) {
            if (fwrite(i2s_readraw_buff, bytes_read, 1, f) != 1) {
                ESP_LOGE(TAG, "Write failed at %d bytes", flash_wr_size);
                break;
            }
            flash_wr_size += bytes_read;
        }
    }

    ESP_LOGI(TAG, "Recording done! Wrote %d bytes to %s", flash_wr_size, filepath);
    fclose(f);
    unmount_sdcard();
}

void init_microphone(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(CONFIG_EXAMPLE_SAMPLE_RATE),
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
    ESP_LOGI(TAG, "Starting recording for category: %s", category_name);
    
    // Initialize microphone
    init_microphone();

    // Record audio
    record_wav(CONFIG_EXAMPLE_REC_TIME, category_name);

    // Cleanup microphone
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }

    // Free category name
    free((void*)category_name);
    
    vTaskDelete(NULL);
}