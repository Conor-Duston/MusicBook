#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/sdspi_host.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_vfs.h"

//SD card file system definitions
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define MOUNT_POINT "/sdc"

#define MAX_NUM_PAGES 7
#define MAX_FILE_NAME_LENGTH 13

void app_main() {

    char *ourTaskName = pcTaskGetName(NULL);
    
    ESP_LOGI(ourTaskName, "Starting up!\n");

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

    f_res = f_opendir(&baseDir , "");
    
    if (f_res != F_OK) {
        ESP_LOGE(ourTaskName, "Could not open directory (%s)\n", mount_point);
        ESP_LOGE(ourTaskName, "FATFS Error: %d", f_res);
        return;
    }

    int file = 0;

    for (;;) {
        f_res = f_readdir(&baseDir, &file_info);
        if (f_res != FR_OK || file_info.fname[0] == 0) break;

        if (file_info.fattrib & AM_DIR) {
            ESP_LOGI(ourTaskName, "Folder: %s", file_info.fname);
        } else {

            ESP_LOGI(ourTaskName, "File Name: %s", file_info.fname);
            ESP_LOGI(ourTaskName, "\tSize: %10lu", file_info.fsize);
            if (file < MAX_NUM_PAGES) {
                strncpy(f_names[file], file_info.fname, MAX_FILE_NAME_LENGTH);
                file ++;
            }
        }
    }
    
    
    f_res = f_closedir(&baseDir);

    if (f_res != F_OK) {
        ESP_LOGE(ourTaskName, "FATFS Error when opening a file: %d", f_res);
        return;
    }
    
    // Sort files by alphanumeric values, IE 0 - 9 a - z, ignoring case, for later retrieval
    qsort(f_names, file, MAX_FILE_NAME_LENGTH, strcasecmp);

    ESP_LOGI(ourTaskName, "File Order: \n");
    for (int i = 0; i <file; ++i) {
        ESP_LOGI(ourTaskName, "%s\n", f_names[i]);
    }

    while (1){
        vTaskDelay (10000);
    }
}