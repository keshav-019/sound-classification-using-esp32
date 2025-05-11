/**
 * @file i2s_recorder_main.c
 * @brief Audio recording and processing implementation
 * 
 * This file handles:
 * - PDM microphone initialization and audio recording
 * - SD card storage management
 * - WAV file creation
 * - Audio feature extraction (MFCC)
 * - File system operations for audio recordings
 */

#include "i2s_recorder_main.h"

static const char *TAG = "pdm_rec_example";

// Audio configuration constants
#define CONFIG_EXAMPLE_BIT_SAMPLE       16      ///< Audio bit depth (16-bit)
#define SPI_DMA_CHAN                    SPI_DMA_CH_AUTO  ///< SPI DMA channel
#define NUM_CHANNELS                    (1)     ///< Mono audio recording
#define SD_MOUNT_POINT                  "/sdcard"  ///< SD card mount point
#define SAMPLE_SIZE                     (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)  ///< Sample buffer size
#define BYTE_RATE                       (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS  ///< Audio byte rate
#define CONFIG_EXAMPLE_REC_TIME         5       ///< Default recording duration (seconds)

// Audio processing constants
#define frame_size 1024                 ///< FFT frame size
#define stride 128                      ///< Frame stride for overlapping
#define SAMPLING_RATE 16000             ///< Audio sampling rate (Hz)
#define record_duration 5               ///< Recording duration (seconds)
#define MAX_BUFFER_SIZE 1024            ///< Maximum processing buffer size
#define num_mfcc 15                     ///< Number of MFCC coefficients to extract
#define NUM_MEL_BINS 23                 ///< Number of Mel filter banks
#define NUM_MFCC_COEFFS 13              ///< Number of MFCC coefficients to keep
#define WAVE_HEADER_SIZE 44             ///< Size of WAV file header
#define FFT_BIN_SIZE (SAMPLING_RATE / 2) / (frame_size / 2)

#define NUM_SAMPLES 1024
#define SAMPLE_SIZE (NUM_SAMPLES * sizeof(int16_t))

// Global variables
sdmmc_host_t host = SDSPI_HOST_DEFAULT();  ///< SD card host configuration
sdmmc_card_t *card;                       ///< SD card handle
static i2s_chan_handle_t rx_handle = NULL;       ///< I2S microphone handle
bool sd_card_mounted = false;             ///< SD card mount status

// Audio processing buffers (aligned for DMA)
__attribute__((aligned(16)))
static float x1[frame_size];  ///< Input buffer 1
__attribute__((aligned(16)))
static float x2[frame_size];  ///< Input buffer 2
__attribute__((aligned(16)))
static float wind[frame_size];  ///< Window coefficients
__attribute__((aligned(16)))
static float diff_y[frame_size / 2];  ///< FFT difference buffer

// Audio feature extraction buffers
static float mel_filter_bank[NUM_MEL_BINS][frame_size / 2];  ///< Mel filter bank
static float mel_spectrum[NUM_MEL_BINS];  ///< Mel spectrum
static float log_mel_spectrum[NUM_MEL_BINS];  ///< Log Mel spectrum
static float dct_matrix[NUM_MEL_BINS][NUM_MFCC_COEFFS];  ///< DCT matrix
static float mfcc[NUM_MFCC_COEFFS];  ///< MFCC coefficients
size_t bytes_read;

// Audio recording buffer
static int16_t i2s_readraw_buff[SAMPLE_SIZE];  ///< Raw audio sample buffer

/**
* @brief Generates Mel filter bank for MFCC computation
* 
* Creates a triangular filter bank in the Mel frequency scale:
* 1. Defines frequency range (0 to Nyquist)
* 2. Converts frequencies to Mel scale
* 3. Creates overlapping triangular filters
*/
void generate_mel_filter_bank() {
    float f_min = 0; // Minimum frequency (Hz)
    float f_max = SAMPLING_RATE / 2; // Nyquist frequency (Hz)

    // Convert frequency range to Mel scale and create filters
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        float f_mel = 2595 * log10(1 + (f_min + (f_max - f_min) * m / (NUM_MEL_BINS - 1)) / 700);
        int lower_bin = (int)((f_mel - 2595 * log10(1 + f_min / 700)) / FFT_BIN_SIZE);
        int upper_bin = (int)((f_mel - 2595 * log10(1 + f_max / 700)) / FFT_BIN_SIZE);
        
        // Create triangular filter
        for (int i = lower_bin; i <= upper_bin; i++) {
            mel_filter_bank[m][i] = 1.0;
        }
    }
}

/**
* @brief Computes Mel spectrum from FFT results
* @param fft_result Pointer to FFT output array
* 
* Steps:
* 1. Multiply FFT bins with Mel filter bank
* 2. Sum energy in each Mel band
*/
void compute_mel_spectrum(float* fft_result) {
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        mel_spectrum[m] = 0.0;
        for (int f = 0; f < frame_size / 2; f++) {
            mel_spectrum[m] += fft_result[f] * mel_filter_bank[m][f];
        }
    }
}

/**
* @brief Applies log compression to Mel spectrum
* 
* Adds small constant (1e-10) to avoid log(0)
*/
void apply_log_to_mel_spectrum() {
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        log_mel_spectrum[m] = log10(fmax(mel_spectrum[m], 1e-10));
    }
}

/**
* @brief Computes DCT of log Mel spectrum to get MFCCs
* 
* Uses Type-II DCT with orthogonal normalization
*/
void apply_dct() {
    for (int c = 0; c < NUM_MFCC_COEFFS; c++) {
        mfcc[c] = 0.0;
        for (int m = 0; m < NUM_MEL_BINS; m++) {
            double temp = log_mel_spectrum[m] * cos(M_PI * c * (2 * m + 1) / (2.0 * NUM_MEL_BINS));
            mfcc[c] += temp;
        }
        // Normalize first coefficient
        if (c == 0) {
            mfcc[c] /= sqrt(2.0);
        }
    }
}

/**
* @brief Mounts SD card with SPI interface
* 
* Steps:
* 1. Configures SPI bus
* 2. Initializes SD card interface
* 3. Mounts FAT filesystem
* 
* @note Automatically formats card if mount fails
*/
void mount_sdcard(void) {
    esp_err_t ret;
    
    // Unmount first if already mounted
    if (sd_card_mounted) {
        unmount_sdcard();
    }

    // Filesystem mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };
    ESP_LOGI(TAG, "Initializing SD card");

    // SPI bus configuration
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_EXAMPLE_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_EXAMPLE_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    // Initialize SPI bus
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus: %s", esp_err_to_name(ret));
        return;
    }

    // SD card device configuration
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_EXAMPLE_SPI_CS_GPIO;
    slot_config.host_id = host.slot;

    // Mount filesystem
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return;
    }

    // Print card info and set mounted flag
    sdmmc_card_print_info(stdout, card);
    sd_card_mounted = true;
}


/**
* @brief Mounts SD card with SPI interface
* 
* Steps:
* 1. Unmounts SD Card
* 2. Unmounts FAT filesystem
* 3. SPI Bus is Freed
* 
* @note Automatically formats card if mount fails
*/
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

/**
* @brief Records audio to WAV file
* @param rec_time Recording duration in seconds
* @param category_name Directory name for storage
* 
* Steps:
* 1. Creates category directory if needed
* 2. Generates unique filename
* 3. Writes WAV header
* 4. Streams audio data from I2S to file
* 5. Closes file when complete
*/
// sumanshu code
void get_audio_samples(int16_t* input_data)
{
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 5000);
    
    // Optional delay before starting recording
    // vTaskDelay(pdMS_TO_TICKS(5000));
    // hadling the garbage values that the sensor produces in the begining
    // skipping the first 10 seconds to deal with sensor shoot up in the begining
    for(int i=0;i<625;i++){
        i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 1000);
    }
    if (ret == ESP_OK && bytes_read == SAMPLE_SIZE) {
        for (int i = 0; i < NUM_SAMPLES; i++) {
            printf("%d ", i2s_readraw_buff[i]);
            input_data[i] = i2s_readraw_buff[i];  // Copy to input_data if needed
        }
    } else {
        printf("Warning: Only %d bytes read (expected %d)\n", (int)bytes_read, SAMPLE_SIZE);
    }
    // Optional delay after finishing recording
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
}

void record_wav(uint32_t rec_time, const char* category_name) {
    mount_sdcard();

    int flash_wr_size = 0;
    ESP_LOGI(TAG, "Opening file");

    // Calculate total bytes to record
    uint32_t flash_rec_time = BYTE_RATE * rec_time;
    
    // Generate WAV header
    const wav_header_t wav_header =
        WAV_HEADER_PCM_DEFAULT(flash_rec_time, 16, CONFIG_EXAMPLE_SAMPLE_RATE, 1);

    // Create category directory
    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", SD_MOUNT_POINT, category_name);
    if(!sd_card_dir_exists(dirpath)){
        mkdir(dirpath, 0777);
    }

    // Get next available recording number
    int recording_num = count_files_in_directory(dirpath) + 1;
    ESP_LOGI("TAG", "Recording number: %d", recording_num);
    
    // Create full filepath
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s/rec_%d.wav", SD_MOUNT_POINT, category_name, recording_num);

    // Open file for writing
    FILE *f = fopen(filepath, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        unmount_sdcard();
        return;
    }

    // Write WAV header
    if (fwrite(&wav_header, sizeof(wav_header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(f);
        unmount_sdcard();
        return;
    }

    // Record audio data
    while (flash_wr_size < flash_rec_time) {
        if (i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 1000) == ESP_OK) {
            if (fwrite(i2s_readraw_buff, bytes_read, 1, f) != 1) {
                ESP_LOGE(TAG, "Write failed at %d bytes", flash_wr_size);
                break;
            }
        printf("Sumanshu:Record audio is running \n");
            flash_wr_size += bytes_read;
        }
    }

    ESP_LOGI(TAG, "Recording complete: %d bytes to %s", flash_wr_size, filepath);
    fclose(f);
    unmount_sdcard();
}

/**
* @brief Initializes PDM microphone
* 
* Configures:
* - I2S channel
* - PDM receiver mode
* - GPIO pins
* - Clock settings
*/
void init_microphone(void) {
    // Channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // PDM receiver configuration
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
    
    // Initialize and enable channel
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

/**
* @brief Main recording task
* @param category_name Audio category name for storage
* 
* Workflow:
* 1. Initializes microphone
* 2. Records audio to SD card
* 3. Cleans up resources
* 4. Deletes task when complete
*/
void start_recording(const char* category_name) {
    ESP_LOGI(TAG, "Starting recording for: %s", category_name);
    
    // Initialize hardware
    init_microphone();
    record_wav(CONFIG_EXAMPLE_REC_TIME, category_name);

    // Cleanup
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }

    free((void*)category_name);
    vTaskDelete(NULL);
}