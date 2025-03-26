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

#define SPI_DMA_CHAN            SPI_DMA_CH_AUTO
#define NUM_CHANNELS            (1) // For mono recording only!
#define SD_MOUNT_POINT          "/sdcard"
#define SAMPLE_SIZE             (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE               (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS
#define CONFIG_EXAMPLE_REC_TIME 15

void mount_sdcard();

void record_wav(uint32_t rec_time);

void init_microphone();
