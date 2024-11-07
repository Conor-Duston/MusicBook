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

void app_main() {

    char *ourTaskName = pcTaskGetName(NULL);
    
    ESP_LOGI(ourTaskName, "Starting up!\n");

    //Initialize SD card
    ESP_LOGI(ourTaskName, "Initializing SD card");

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .disk_status_check_enable = true,
        .max_files = 2
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

    while (1){
        vTaskDelay (10000);
    }
}