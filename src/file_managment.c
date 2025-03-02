#include "file_managment.h"

#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

static const char* ourTaskName = "file_management";

static char files[NUM_SECTIONS][MAX_FILE_NAME_LENGTH];

bool mount_fs(sdmmc_card_t *card) {
    /*
        SD card section:
        Initialize SPI Communication bus
        Initialize SD card
        Mount FAT file system
        Read and sort files
    */ 

  // Initialize SD card
  ESP_LOGI(ourTaskName, "Initializing SD card");

  esp_vfs_fat_mount_config_t mount_config = {.format_if_mount_failed = true,
                                             .disk_status_check_enable = true,
                                             .max_files = 1,
                                             .allocation_unit_size = 4096};

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // Speed set at about .7 max speed that did not through a 109 improper error.
  host.max_freq_khz = 5000;
  
  spi_bus_config_t bus_cfg =  {.mosi_io_num = PIN_NUM_MOSI,
                              .miso_io_num = PIN_NUM_MISO,
                              .sclk_io_num = PIN_NUM_CLK,
                              .quadhd_io_num = -1,
                              .quadwp_io_num = -1};

  esp_err_t ret;
  ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  
  if (ret != ESP_OK)
  {
    ESP_LOGE(ourTaskName, "Failed to initialize bus");
    return false;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;

  ESP_LOGI(ourTaskName, "Mounting FS");

  const char mount_point[] = MOUNT_POINT;

  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config,
                                &card);

  if (ret != ESP_OK)
  {
    if (ret == ESP_FAIL)
    {
      ESP_LOGE(ourTaskName, "Failed to mount fs");
    }
    else
    {
      ESP_LOGE(ourTaskName,
               "Failed to initialize the card (%s). Make sure to have pull-up "
               "resistors in place.",
               esp_err_to_name(ret));
    }
    return false;
  }

  ESP_LOGI(ourTaskName, "FS mounted");

  sdmmc_card_print_info(stdout, card);

  return true;
}


void sort_filenames () {
  FF_DIR baseDir;
  FILINFO file_info;

  FRESULT f_res;
  
  // Open directory were SD card resides (base directory)
  // Use FATFS file functions, not ANSI C
  f_res = f_opendir(&baseDir, "");

  if (f_res != F_OK)
  {
    ESP_LOGE(ourTaskName, "Could not open directory (%s)\n", MOUNT_POINT);
    ESP_LOGE(ourTaskName, "FATFS Error: %d", f_res);
    return;
  }

  int file = 0;

  // Read all files in and directories, only add files to list
  for (;;)
  {
    f_res = f_readdir(&baseDir, &file_info);
    if (f_res != FR_OK || file_info.fname[0] == 0)
      break;

    if (file_info.fattrib & AM_DIR)
    {
      ESP_LOGI(ourTaskName, "Folder: %s", file_info.fname);
    }
    else
    {

      ESP_LOGI(ourTaskName, "File Name: %s", file_info.fname);
      ESP_LOGI(ourTaskName, "\tSize: %10lu", file_info.fsize);
      if (file >= NUM_SECTIONS)
      {
        continue;
      }

      // Search string for wav type. if type is present, then sub_address will
      // not be null
      char *sub_address =
          strnstr(file_info.fname, ".WAV", MAX_FILE_NAME_LENGTH);

      if (file < NUM_SECTIONS && sub_address != NULL)
      {
        strcpy(files[file], file_info.fname);
        ESP_LOGI(ourTaskName, "File Name Copy: %s", files[file]);
        file++;
      }
    }
  }

  // Close directory to save on resources
  f_res = f_closedir(&baseDir);

  if (f_res != F_OK)
  {
    ESP_LOGE(ourTaskName, "FATFS Error when opening a file: %d", f_res);
    return;
  }

  ESP_LOGI(ourTaskName, "File Order pre sort: \n");
  for (int i = 0; i < file; ++i)
  {
    ESP_LOGI(ourTaskName, "%s\n", files[i]);
  }

  for (int i = 0; i < file; i++) {
    for (int j = i + 1; j < file; j++) {
      if (strcmp(files[i], files[j]) > 0) {
        char temp[MAX_FILE_NAME_LENGTH];
        strcpy(temp, files[i]);
        strcpy(files[i], files[j]);
        strcpy(files[j], temp);
      }
    }

  }
 
  ESP_LOGI(ourTaskName, "File Order: \n");
  for (int i = 0; i < file; ++i)
  {
    ESP_LOGI(ourTaskName, "%s\n", files[i]);
  }
}

int open_file(const int index, TinyWav* tiny_wav_output) {
    // File opening section:
  // Open file for reading

  // file_name length is size of mount point plus maximum size of file name
  char file_name[sizeof(MOUNT_POINT) + MAX_FILE_NAME_LENGTH];
  strcpy(file_name, MOUNT_POINT);
  strncat(file_name, "/", 2);
  strncat(file_name, files[0], sizeof(file_name) - 1);

  ESP_LOGI(ourTaskName, "File to open:  %s", file_name);

  int err = tinywav_open_read(tiny_wav_output, file_name, TW_INTERLEAVED);
  
  if (err != 0)
  {
    ESP_LOGE(ourTaskName, "Tiny wave could not open file to read.");
    ESP_LOGE(ourTaskName, "Error: %d", err);
  }
  return err;
}