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
// #define SAMPLE_SIZE                     (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)  ///< Sample buffer size
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
// static float dct_matrix[NUM_MEL_BINS][NUM_MFCC_COEFFS];  ///< DCT matrix
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
            flash_wr_size += bytes_read;
        }
    }

    ESP_LOGI(TAG, "Recording complete: %d bytes to %s", flash_wr_size, filepath);
    fclose(f);
    unmount_sdcard();
}

// MFCC Configuration
#define FRAME_LENGTH_MS 25
#define FRAME_SHIFT_MS 10
#define N_MELS 20

// Derived constants
#define FRAME_LENGTH (SAMPLE_RATE * FRAME_LENGTH_MS / 1000)
#define FRAME_SHIFT (SAMPLE_RATE * FRAME_SHIFT_MS / 1000)
#define PREEMPHASIS_COEFF 0.97f

// Allocate large buffers in external RAM (PSRAM) if available
#if CONFIG_SPIRAM_USE_MALLOC
static float *hamming_window = NULL;
static float *mel_filterbank = NULL;
static float *dct_matrix = NULL;
#else
static float hamming_window[FRAME_LENGTH] = {0};
static float mel_filterbank[N_MELS][N_FFT/2 + 1] = {0};
static float dct_matrix[N_MFCC][N_MELS] = {0};
#endif

// Convert Hz to Mel scale
static float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

// Initialize MFCC processing (call once at startup)
void init_mfcc() {
    // Initialize memory for large buffers
    #if CONFIG_SPIRAM_USE_MALLOC
    hamming_window = (float*)heap_caps_malloc(FRAME_LENGTH * sizeof(float), MALLOC_CAP_SPIRAM);
    mel_filterbank = (float*)heap_caps_malloc(N_MELS * (N_FFT/2 + 1) * sizeof(float), MALLOC_CAP_SPIRAM);
    dct_matrix = (float*)heap_caps_malloc(N_MFCC * N_MELS * sizeof(float), MALLOC_CAP_SPIRAM);
    
    if (!hamming_window || !mel_filterbank || !dct_matrix) {
        ESP_LOGE(TAG, "Failed to allocate MFCC buffers in PSRAM");
        return;
    }
    #endif

    // Initialize Hamming window
    for (int i = 0; i < FRAME_LENGTH; i++) {
        hamming_window[i] = 0.54f - 0.46f * cosf(2 * M_PI * i / (FRAME_LENGTH - 1));
    }

    // Initialize Mel filterbank
    float mel_min = hz_to_mel(0);
    float mel_max = hz_to_mel(SAMPLE_RATE / 2);
    float mel_points[N_MELS + 2];
    
    for (int i = 0; i < N_MELS + 2; i++) {
        mel_points[i] = mel_min + i * (mel_max - mel_min) / (N_MELS + 1);
    }
    
    for (int i = 0; i < N_MELS; i++) {
        for (int j = 0; j < N_FFT/2 + 1; j++) {
            float freq = (float)j * SAMPLE_RATE / N_FFT;
            float mel = hz_to_mel(freq);
            
            if (mel < mel_points[i]) {
                mel_filterbank[i][j] = 0;
            } else if (mel <= mel_points[i+1]) {
                mel_filterbank[i][j] = (mel - mel_points[i]) / (mel_points[i+1] - mel_points[i]);
            } else if (mel <= mel_points[i+2]) {
                mel_filterbank[i][j] = (mel_points[i+2] - mel) / (mel_points[i+2] - mel_points[i+1]);
            } else {
                mel_filterbank[i][j] = 0;
            }
        }
    }

    // Initialize DCT matrix (Type-II DCT with orthogonal normalization)
    for (int k = 0; k < N_MFCC; k++) {
        for (int n = 0; n < N_MELS; n++) {
            dct_matrix[k][n] = sqrtf(2.0f/N_MELS) * cosf(M_PI * k * (2*n + 1) / (2 * N_MELS));
        }
    }
    // Special case for k=0
    for (int n = 0; n < N_MELS; n++) {
        dct_matrix[0][n] *= sqrtf(2.0f)/2.0f;
    }
}

// Extract MFCC features from audio samples
void extract_mfcc_features(int16_t* audio_samples, float* mfcc_output) {
    float frame[FRAME_LENGTH];
    float fft_buffer[N_FFT] = {0};
    float power_spectrum[N_FFT/2 + 1];
    float mel_energies[N_MELS];
    float mfcc_frame[N_MFCC];
    
    int num_frames = (1024 - FRAME_LENGTH) / FRAME_SHIFT + 1;
    if (num_frames > 1024) num_frames = 1024; // Safety check
    
    // Pre-emphasis
    float prev_sample = audio_samples[0] / 32768.0f;
    for (int i = 1; i < 1024; i++) {
        float sample = audio_samples[i] / 32768.0f;
        audio_samples[i] = (sample - PREEMPHASIS_COEFF * prev_sample) * 32768.0f;
        prev_sample = sample;
    }

    // Process each frame
    for (int frame_idx = 0; frame_idx < num_frames; frame_idx++) {
        int offset = frame_idx * FRAME_SHIFT;
        
        // Apply Hamming window
        for (int i = 0; i < FRAME_LENGTH; i++) {
            if (offset + i < 1024) {
                frame[i] = (audio_samples[offset + i] / 32768.0f) * hamming_window[i];
            } else {
                frame[i] = 0; // Zero-pad if needed
            }
        }

        // Compute FFT
        memcpy(fft_buffer, frame, FRAME_LENGTH * sizeof(float));
        dsps_fft2r_init_fc32(NULL, N_FFT);
        dsps_fft2r_fc32(fft_buffer, N_FFT);
        dsps_bit_rev2r_fc32(fft_buffer, N_FFT);
        
        // Compute power spectrum
        for (int i = 0; i < N_FFT/2 + 1; i++) {
            float real = fft_buffer[i*2];
            float imag = fft_buffer[i*2 + 1];
            power_spectrum[i] = (real * real + imag * imag) / N_FFT;
        }

        // Apply Mel filterbank
        memset(mel_energies, 0, sizeof(mel_energies));
        for (int i = 0; i < N_MELS; i++) {
            for (int j = 0; j < N_FFT/2 + 1; j++) {
                mel_energies[i] += power_spectrum[j] * mel_filterbank[i][j];
            }
            // Log of energy
            mel_energies[i] = logf(mel_energies[i] + 1e-6f); // Add small offset to avoid log(0)
        }

        // Compute DCT manually using matrix multiplication
        for (int k = 0; k < N_MFCC; k++) {
            mfcc_frame[k] = 0.0f;
            for (int n = 0; n < N_MELS; n++) {
                mfcc_frame[k] += mel_energies[n] * dct_matrix[k][n];
            }
        }
        
        // Copy to output buffer (N_MFCC coefficients per frame)
        for (int i = 0; i < N_MFCC; i++) {
            mfcc_output[frame_idx * N_MFCC + i] = mfcc_frame[i];
        }
    }

    // If we have fewer than 1024 frames, zero-pad the remaining
    for (int i = num_frames * N_MFCC; i < 1024 * N_MFCC; i++) {
        mfcc_output[i] = 0.0f;
    }
}


// Don't forget to free allocated memory when done
void deinit_mfcc() {
    #if CONFIG_SPIRAM_USE_MALLOC
    if (hamming_window) heap_caps_free(hamming_window);
    if (mel_filterbank) heap_caps_free(mel_filterbank);
    if (dct_matrix) heap_caps_free(dct_matrix);
    #endif
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
    // Check if already initialized
    if (rx_handle != NULL) {
        return;
    }

    // Channel configuration - explicitly use I2S_NUM_0
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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

void deinit_microphone(void) {
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
}

// sumanshu code
void get_audio_samples(int16_t* input_data)
{
    assert(input_data != NULL);  // Ensure valid pointer
    assert((uintptr_t)input_data % 4 == 0);  // Ensure 4-byte alignment
    
    // Skip initial garbage samples (more efficient version)
    const int skip_samples = 625;
    size_t bytes_read = 0;
    
    for(int i=0; i<skip_samples; i++){
        if(i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, 
                          SAMPLE_SIZE, &bytes_read, 100) != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed during warmup");
            return;
        }
    }

    // Get actual samples
    esp_err_t ret = i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, 
                                   SAMPLE_SIZE, &bytes_read, 5000);
    
    if (ret == ESP_OK && bytes_read == SAMPLE_SIZE) {
        // Replace printf with ESP_LOG at most verbose level
        ESP_LOGV(TAG, "Samples:");
        for (int i = 0; i < NUM_SAMPLES; i++) {
            ESP_LOGV(TAG, "%d", i2s_readraw_buff[i]);
            input_data[i] = i2s_readraw_buff[i];
        }
    } else {
        ESP_LOGE(TAG, "I2S read failed: %d bytes (expected %d)", 
                bytes_read, SAMPLE_SIZE);
    }
}

/**
 * @brief Collects 1024 audio samples from I2S microphone
 * @param audio_buffer Output buffer for 16-bit PCM samples (must be 1024 elements)
 * @return ESP_OK on success, error code on failure
 * 
 * @note This function:
 * - Requires initialized I2S microphone
 * - Blocks for ~64ms (at 16kHz sampling rate)
 * - Automatically retries once on read failure
 */
esp_err_t collect_audio_samples(int16_t *audio_buffer) {
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Invalid audio buffer");
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_read;
    esp_err_t ret;

    if (rx_handle == NULL) {
        init_microphone(); // Make sure this sets rx_handle properly
        // Add error check
        if (rx_handle == NULL) {
            ESP_LOGE(TAG, "Microphone initialization failed!");
            return ESP_FAIL;
        }
    }
    
    // Try reading with 1s timeout (may need multiple attempts)
    for (int attempt = 0; attempt < 2; attempt++) {
        ret = i2s_channel_read(rx_handle, 
                              (char *)audio_buffer, 
                              1024 * sizeof(int16_t),
                              &bytes_read, 
                              1000 / portTICK_PERIOD_MS);
        
        if (ret == ESP_OK && bytes_read == 1024 * sizeof(int16_t)) {
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "Audio read attempt %d failed: %s (bytes: %d)", 
                attempt, esp_err_to_name(ret), bytes_read);
    }

    ESP_LOGE(TAG, "Failed to collect 1024 samples");
    return ESP_FAIL;
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