#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/sdspi_host.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_vfs.h"

#include "driver/i2s_std.h"

//SD card file system definitions
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define MOUNT_POINT "/sdc"

#define MAX_NUM_PAGES 7
#define MAX_FILE_NAME_LENGTH 13

// I2S driver and output
#define I2S_CLK_PIN 22 
#define I2S_DOUT 1 //Labeled "TX" on  ESP32 board
#define I2S_WS 3 //Labeled "RX" on  ESP32 board

#define WB_SIZE 2048



void app_main() {

    char *ourTaskName = pcTaskGetName(NULL);
    
    ESP_LOGI(ourTaskName, "Starting up!\n");

    /*
        SD card section:
        Initialize SPI Communication bus
        Initialize SD card
        Mount FAT file system
        Read and sort files
    */

    //Initialize SD card
    ESP_LOGI(ourTaskName, "Initializing SD card");

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .disk_status_check_enable = true,
        .max_files = 2,
        .allocation_unit_size = 4096
    };

    sdmmc_card_t *card;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    //Speed set at about .7 max speed that did not through a 109 improper error.
    host.max_freq_khz = 6000;
    

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1
    };

    esp_err_t ret;
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(ourTaskName, "Failed to initialize bus");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(ourTaskName, "Mounting FS");

    const char mount_point[] = MOUNT_POINT;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(ourTaskName, "Failed to mount fs");
        } else {
            ESP_LOGE(ourTaskName, "Failed to initialize the card (%s). Make sure to have pull-up resistors in place.", 
                    esp_err_to_name(ret));
        }
        return;
    }

    ESP_LOGI(ourTaskName, "FS mounted");

    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(ourTaskName, "Opening Base Directory and Searching for files");
    
    FF_DIR baseDir;
    FILINFO file_info;

    FRESULT f_res;

    TCHAR f_names[MAX_NUM_PAGES][MAX_FILE_NAME_LENGTH];

    // Open directory were SD card resides (base directory)
    // Use FATFS file functions, not ANSI C 
    f_res = f_opendir(&baseDir , "");
    
    if (f_res != F_OK) {
        ESP_LOGE(ourTaskName, "Could not open directory (%s)\n", mount_point);
        ESP_LOGE(ourTaskName, "FATFS Error: %d", f_res);
        return;
    }

    int file = 0;


    // Read all files in and directories, only add files to list
    for (;;) {
        f_res = f_readdir(&baseDir, &file_info);
        if (f_res != FR_OK || file_info.fname[0] == 0) break;

        if (file_info.fattrib & AM_DIR) {
            ESP_LOGI(ourTaskName, "Folder: %s", file_info.fname);
        } else {

            ESP_LOGI(ourTaskName, "File Name: %s", file_info.fname);
            ESP_LOGI(ourTaskName, "\tSize: %10lu", file_info.fsize);
            if (file < MAX_NUM_PAGES) {
                strcpy(f_names[file], file_info.fname);
                ESP_LOGI(ourTaskName, "File Name Copy: %s", f_names[file]);
                file ++;
            }
        }
    }
    
    //Close directory to save on resources
    f_res = f_closedir(&baseDir);

    if (f_res != F_OK) {
        ESP_LOGE(ourTaskName, "FATFS Error when opening a file: %d", f_res);
        return;
    }
    
    // Sort files by alphanumeric values, IE 0 - 9a - z, ignoring case, for later retrieval
    qsort(f_names, file, sizeof(f_names[0]), strcasecmp);

    ESP_LOGI(ourTaskName, "File Order: \n");
    for (int i = 0; i <file; ++i) {
        ESP_LOGI(ourTaskName, "%s\n", f_names[i]);
    }

    // /*
    //     I2S section
    //     initialize I2S driver
    // */
    i2s_chan_handle_t tx_handle;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_CLK_PIN,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,

            },
        },
        
    };
    
    //ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));

    // // Write some data to the channel output

    // uint8_t *w_buf = (uint8_t *)calloc(1, WB_SIZE);
    // assert(w_buf);

    // for (int i = 0; i < WB_SIZE; i += 8) {
    //     w_buf[i]     = 0x12;
    //     w_buf[i + 1] = 0x34;
    //     w_buf[i + 2] = 0x56;
    //     w_buf[i + 3] = 0x78;
    //     w_buf[i + 4] = 0x9A;
    //     w_buf[i + 5] = 0xBC;
    //     w_buf[i + 6] = 0xDE;
    //     w_buf[i + 7] = 0xF0;
    // }

    // size_t w_bytes = WB_SIZE;

    // while (w_bytes == WB_SIZE) {
    //     ESP_ERROR_CHECK(i2s_channel_preload_data(tx_handle, w_buf, WB_SIZE, &w_bytes));
    // }

    // ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    // while (1)
    // {
    //     if (i2s_channel_write(tx_handle, w_buf, WB_SIZE, &w_bytes, 1000) == ESP_OK) {
    //         ESP_LOGI(ourTaskName, "i2s write %d bytes\n", w_bytes);
    //     }  else {
    //         ESP_LOGI(ourTaskName, "i2s write failed\n");
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(200));
    // }

    // free(w_buf);

    

    while (1){
        vTaskDelay (10000);
    }
}