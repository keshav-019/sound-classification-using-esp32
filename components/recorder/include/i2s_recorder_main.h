#pragma once

/* I2S Digital Microphone Recording Example */
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


// MFCC configuration
#define N_FFT 8192
#define N_MEL_BANKS 13
#define N_MFCC 10
#define SAMPLE_RATE CONFIG_EXAMPLE_SAMPLE_RATE
#define SD_MOUNT_POINT "/sdcard"

typedef struct {
    float* mel_fb;
    float* dct_matrix;
    float* window;
    float* fft_input;
    float* power_spectrum;
    float* mel_energies;
    int n_fft;
    int n_mels;
    int n_mfcc;
} mfcc_processor_t;

// Add these declarations
extern sdmmc_host_t host;
extern sdmmc_card_t *card;
extern bool sd_card_mounted;

void setup_mfcc(mfcc_processor_t* mfcc_processor);
void create_mel_filterbank(float* mel_fb, int n_fft, int n_mels, int sample_rate);
void apply_mel_filterbank(float* power_spectrum, float* mel_fb, float* mel_energies, int n_fft, int n_mels);
void mount_sdcard(void);
void record_wav(uint32_t rec_time, const char* category_name);
void init_microphone(void);
void start_recording(const char* category_name);
void unmount_sdcard(void);
esp_err_t collect_audio_samples(int16_t *audio_buffer);