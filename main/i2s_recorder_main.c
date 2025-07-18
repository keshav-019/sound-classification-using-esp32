/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* I2S Digital Microphone Recording Example */
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
#include <stdint.h>

static const char *TAG = "pdm_rec_example";

#define SPI_DMA_CHAN        SPI_DMA_CH_AUTO
#define SD_MOUNT_POINT      "/sdcard"

// When testing SD and SPI modes, keep in mind that once the card has been
// initialized in SPI mode, it can not be reinitialized in SD mode without
// toggling power to the card.
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;



// ******************************************************* //

#define SAMPLE_RATE 16000
#define FRAME_SIZE     400     // 25ms @ 16kHz
#define FRAME_SHIFT    200     // 50% overlap
#define FFT_SIZE       512
#define MEL_FILTERS    26
#define MFCC_COEFFS    13
#define PI             3.14159265358979323846f

// ******************************************************* //

void pre_emphasis(const float *in, float *out, int n, float alpha) {
    out[0] = in[0];
    for (int i = 1; i < n; i++)
        out[i] = in[i] - alpha * in[i - 1];
}

void apply_hamming(float *frame, int n) {
    for (int i = 0; i < n; i++)
        frame[i] *= 0.54f - 0.46f * cosf(2.0f * PI * i / (n - 1));
}

void fft(float *re, float *im, int n) {
    int j = 0;
    for (int i = 1; i < n - 1; i++) {
        int bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float tr = re[i], ti = im[i];
            re[i] = re[j]; im[i] = im[j];
            re[j] = tr;     im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * PI / len;
        float wlen_re = cosf(ang), wlen_im = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float ur = 1.0f, ui = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int even = i + j;
                int odd = i + j + len / 2;
                float vr = re[odd] * ur - im[odd] * ui;
                float vi = re[odd] * ui + im[odd] * ur;
                re[odd] = re[even] - vr;
                im[odd] = im[even] - vi;
                re[even] += vr;
                im[even] += vi;
                float next_ur = ur * wlen_re - ui * wlen_im;
                ui = ur * wlen_im + ui * wlen_re;
                ur = next_ur;
            }
        }
    }
}

float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

void create_mel_filters(int num_filters, int fft_size, int sample_rate,
                        float mel_filters[][FFT_SIZE/2+1]) {
    float min_m = hz_to_mel(0.0f);
    float max_m = hz_to_mel(sample_rate / 2.0f);
    float step = (max_m - min_m) / (num_filters + 1);
    float mels[MEL_FILTERS + 2], fts[MEL_FILTERS + 2];
    for (int i = 0; i < num_filters + 2; i++) {
        mels[i] = min_m + step * i;
        fts[i] = mel_to_hz(mels[i]) * (fft_size / 2.0f) / (sample_rate / 2.0f);
    }

    for (int i = 0; i < num_filters; i++) {
        for (int j = 0; j <= fft_size/2; j++) {
            if (j < fts[i]) mel_filters[i][j] = 0.0f;
            else if (j <= fts[i+1])
                mel_filters[i][j] = (j - fts[i]) / (fts[i+1] - fts[i]);
            else if (j <= fts[i+2])
                mel_filters[i][j] = (fts[i+2] - j) / (fts[i+2] - fts[i+1]);
            else
                mel_filters[i][j] = 0.0f;
        }
    }
}

void dct(const float *in, float *out, int n, int k_num) {
    for (int k = 0; k < k_num; k++) {
        float sum = 0.0f;
        for (int i = 0; i < n; i++)
            sum += in[i] * cosf(PI * k * (2*i + 1) / (2.0f * n));
        out[k] = sum;
    }
}


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

void app_main(void)
{
    mount_sdcard();

    FILE *f = fopen(SD_MOUNT_POINT "/BELL.WAV", "rb");
    if (!f) {
        printf("Failed to open WAV file.\n");
        return;
    }

    // Skip WAV header
    fseek(f, 44, SEEK_SET);

    // Pre-allocate buffers (with error checks)
    float *frame      = malloc(FRAME_SIZE * sizeof(float));
    float *emph       = malloc(FRAME_SIZE * sizeof(float));
    float *re         = malloc(FFT_SIZE * sizeof(float));
    float *im         = malloc(FFT_SIZE * sizeof(float));
    float *pow_spec   = malloc((FFT_SIZE / 2 + 1) * sizeof(float));
    float (*mel_filt)[FFT_SIZE / 2 + 1] = malloc(MEL_FILTERS * (FFT_SIZE / 2 + 1) * sizeof(float));

    if (!frame || !emph || !re || !im || !pow_spec || !mel_filt) {
        printf("Memory allocation failed!\n");
        fclose(f);
        free(frame); free(emph); free(re); free(im); free(pow_spec); free(mel_filt);
        return;
    }

    float mel_energies[MEL_FILTERS];
    float mfcc[MFCC_COEFFS];
    float mfcc_sum[MFCC_COEFFS] = {0};
    int num_frames = 0;

    // For reading raw 16-bit PCM samples
    short overlap_buffer[FRAME_SIZE];

    // Create mel filterbank once
    create_mel_filters(MEL_FILTERS, FFT_SIZE, SAMPLE_RATE, mel_filt);

    // Initial read
    size_t read_samples = fread(overlap_buffer, sizeof(short), FRAME_SIZE, f);
    
    while (read_samples == FRAME_SIZE) {
        // Convert to float
        for (int i = 0; i < FRAME_SIZE; i++) {
            frame[i] = (float)overlap_buffer[i];
        }

        pre_emphasis(frame, emph, FRAME_SIZE, 0.97f);
        apply_hamming(emph, FRAME_SIZE);

        // Prepare FFT input
        for (int i = 0; i < FFT_SIZE; i++) {
            re[i] = (i < FRAME_SIZE) ? emph[i] : 0.0f;
            im[i] = 0.0f;
        }

        fft(re, im, FFT_SIZE);

        // Power spectrum
        for (int i = 0; i <= FFT_SIZE / 2; i++) {
            pow_spec[i] = re[i] * re[i] + im[i] * im[i];
        }

        // Apply mel filters and compute log energies
        for (int m = 0; m < MEL_FILTERS; m++) {
            float sum = 0.0f;
            for (int k = 0; k <= FFT_SIZE / 2; k++) {
                sum += pow_spec[k] * mel_filt[m][k];
            }
            mel_energies[m] = logf(sum + 1e-10f);
        }

        // DCT to get MFCCs
        dct(mel_energies, mfcc, MEL_FILTERS, MFCC_COEFFS);

        // Accumulate for mean MFCC
        for (int c = 0; c < MFCC_COEFFS; c++) {
            mfcc_sum[c] += mfcc[c];
        }
        num_frames++;

        // Shift last FRAME_SIZE - FRAME_SHIFT samples
        memmove(overlap_buffer, overlap_buffer + FRAME_SHIFT,
                (FRAME_SIZE - FRAME_SHIFT) * sizeof(short));

        // Read next FRAME_SHIFT samples
        size_t shift_samples = fread(overlap_buffer + (FRAME_SIZE - FRAME_SHIFT),
                                     sizeof(short), FRAME_SHIFT, f);
        read_samples = (FRAME_SIZE - FRAME_SHIFT) + shift_samples;
    }

    fclose(f);

    // Compute mean MFCC vector
    for (int i = 0; i < MFCC_COEFFS; i++) {
        mfcc_sum[i] /= num_frames;
    }

    // Print mean MFCCs
    printf("Mean MFCC:\n");
    for (int i = 0; i < MFCC_COEFFS; i++) {
        printf("%f ", mfcc_sum[i]);
    }
    printf("\n");

    // Free dynamically allocated memory
    free(frame);
    free(emph);
    free(re);
    free(im);
    free(pow_spec);
    free(mel_filt);

    // Optional: keep task alive if needed
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


// void app_main(void)
// {
//     mount_sdcard();

//     FILE *f = fopen(SD_MOUNT_POINT"/BELL.WAV", "rb");
//     if (!f) {
//         printf("Failed to open WAV file.\n");
//         return;
//     }
//     // Skip WAV header
//     fseek(f, 44, SEEK_SET);

//     // Pre-allocate buffers
//     float *frame = malloc(FRAME_SIZE * sizeof(float));
//     float *emph  = malloc(FRAME_SIZE * sizeof(float));
//     float *re = malloc(FFT_SIZE * sizeof(float));
//     float *im = malloc(FFT_SIZE * sizeof(float));
//     float *pow_spec = malloc((FFT_SIZE/2 + 1) * sizeof(float));
//     float (*mel_filt)[FFT_SIZE/2 + 1] = malloc(MEL_FILTERS * (FFT_SIZE/2 + 1) * sizeof(float));
//     float mel_energies[MEL_FILTERS];
//     float mfcc[MFCC_COEFFS];
//     float mfcc_sum[MFCC_COEFFS] = {0};
//     int num_frames = 0;


//     // For reading raw 16-bit PCM samples directly
//     short pcm_buffer[FRAME_SIZE];


//     // Create mel filterbank once
//     create_mel_filters(MEL_FILTERS, FFT_SIZE, SAMPLE_RATE, mel_filt);
//     // adding task delay to tackle watchdog interrupt
//     vTaskDelay(pdMS_TO_TICKS(10));

//     // Read and process one frame at a time with overlap
//     short overlap_buffer[FRAME_SIZE];  // to keep overlap samples
//     size_t read_samples = fread(overlap_buffer, sizeof(short), FRAME_SIZE, f);



//     while (read_samples == FRAME_SIZE) {
//         // Convert to float
//         for (int i = 0; i < FRAME_SIZE; i++) {
//             frame[i] = (float)overlap_buffer[i];
//         }

//         pre_emphasis(frame, emph, FRAME_SIZE, 0.97f);
//         vTaskDelay(pdMS_TO_TICKS(10));
//         apply_hamming(emph, FRAME_SIZE);
//         vTaskDelay(pdMS_TO_TICKS(10));
//         // Prepare FFT input
//         for (int i = 0; i < FFT_SIZE; i++) {
//             re[i] = (i < FRAME_SIZE) ? emph[i] : 0.0f;
//             im[i] = 0.0f;
//         }
//         vTaskDelay(pdMS_TO_TICKS(10));
//         fft(re, im, FFT_SIZE);
//         vTaskDelay(pdMS_TO_TICKS(10));

//         // Power spectrum
//         for (int i = 0; i <= FFT_SIZE / 2; i++) {
//             pow_spec[i] = re[i] * re[i] + im[i] * im[i];
//         }
//         vTaskDelay(pdMS_TO_TICKS(10));

//         // Apply mel filters and compute log energies
//         for (int m = 0; m < MEL_FILTERS; m++) {
//             float sum = 0.0f;
//             for (int k = 0; k <= FFT_SIZE / 2; k++) {
//                 sum += pow_spec[k] * mel_filt[m][k];
//             }
//             mel_energies[m] = logf(sum + 1e-10f);
//         }

//         // DCT to get MFCCs
//         dct(mel_energies, mfcc, MEL_FILTERS, MFCC_COEFFS);
//         vTaskDelay(pdMS_TO_TICKS(10));
//         // Accumulate for mean MFCC
//         for (int c = 0; c < MFCC_COEFFS; c++) {
//             mfcc_sum[c] += mfcc[c];
//         }
//         num_frames++;

//         // Shift last 200 samples to start of buffer (overlap)
//         memmove(overlap_buffer, overlap_buffer + FRAME_SHIFT, (FRAME_SIZE - FRAME_SHIFT) * sizeof(short));

//         // Read next 200 samples
//         read_samples = fread(overlap_buffer + (FRAME_SIZE - FRAME_SHIFT), sizeof(short), FRAME_SHIFT, f);
//         read_samples += (FRAME_SIZE - FRAME_SHIFT);  // total valid samples in overlap_buffer
//         vTaskDelay(pdMS_TO_TICKS(10));  // delay 1 ms (or experiment with a few ms)
//         // break;
//     }
//     printf("While ends \n");

// fclose(f);
// free(frame);
// free(emph);

// // Compute mean MFCC vector
// for (int i = 0; i < MFCC_COEFFS; i++) {
//     mfcc_sum[i] /= num_frames;
// }
//     vTaskDelay(pdMS_TO_TICKS(10));  // delay 1 ms (or experiment with a few ms)

// // Print mean MFCCs
// for (int i = 0; i < MFCC_COEFFS; i++) {
//     printf("%.4f ", mfcc_sum[i]);
// }
// printf("\n");

// free(mel_filt);
// while (1) {
//         vTaskDelay(pdMS_TO_TICKS(10));
//     }

// }
