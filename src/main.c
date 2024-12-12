#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "soc/lldesc.h"

#include "esp_log.h"

#include "driver/sdspi_host.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "driver/i2s_std.h"

#include "tinywav.h"
#include "main.h"

// SD card file system definitions
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define MOUNT_POINT "/sdc"

#define MAX_NUM_PAGES 7
#define MAX_FILE_NAME_LENGTH 13

// I2S driver and output
#define I2S_CLK_PIN 25
#define I2S_DOUT 26
#define I2S_WS 27

#define MIN_DATA_SIZE 96   
#define DATA_MULTIPLIER 16
#define BUFF_SIZE MIN_DATA_SIZE * DATA_MULTIPLIER
#define BUFF_READ_SIZE BUFF_SIZE / 16
#define I2S_BUFF_READ 32

// Structure for holding read and write tasks
struct ReadWrite_t
{
  TaskHandle_t read_task;
  TaskHandle_t write_task;
};

static struct ReadWrite_t audiotasks;

SemaphoreHandle_t audio_info_mutex;
static uint32_t sample_frequency;
static uint16_t bits_sample;
static uint16_t num_channels;
static bool info_changed = false;

SemaphoreHandle_t audio_semaphore;
uint8_t audio_handle[BUFF_SIZE];
uint8_t* current_read = &audio_handle; 
uint8_t* current_write = &audio_handle;



void app_main(void)
{

  char *ourTaskName = pcTaskGetName(NULL);

  ESP_LOGI(ourTaskName, "Starting up!\n");
  ESP_LOGI(ourTaskName, "Task name pointer: %p", ourTaskName);

  audio_info_mutex = xSemaphoreCreateMutex();

  if (audio_info_mutex == NULL)
  {
    ESP_LOGE(ourTaskName, "Could not create required mutex");
  }

  audio_semaphore = xSemaphoreCreateMutex();

  if (audio_info_mutex == NULL) {
    ESP_LOGE(ourTaskName, "could not create mutex for audio handling");
  }

  //audio_handle = xRingbufferCreate(BUFF_SIZE, RINGBUF_TYPE_BYTEBUF);

  if (audio_handle == NULL)
  {
    ESP_LOGE(ourTaskName, "Could not create ring buffer");
  }

  BaseType_t result =
      xTaskCreatePinnedToCore(read_file_to_shared_buffer, "read_file", 8192,
                              NULL, 10, &audiotasks.read_task, 1);
  if (result != pdPASS)
  {
    ESP_LOGE(ourTaskName, "Failed to create write task");
    return;
  }

  result = xTaskCreatePinnedToCore(write_shared_audio_buffer, "play_audio",
                                   8192, NULL, 10, &audiotasks.write_task, 0);

  if (result != pdPASS)
  {
    ESP_LOGE(ourTaskName, "Failed to create read task");
    return;
  }

  while (1)
  {
    vTaskDelay(10000);
    if (audiotasks.read_task == NULL)
    {
      ESP_LOGE(ourTaskName, "Error in read task");
      return;
    }
    else if (audiotasks.write_task == NULL)
    {
      ESP_LOGE(ourTaskName, "Error in write task");
      return;
    }
  }
}

void read_file_to_shared_buffer()
{
  char *ourTaskName = pcTaskGetName(NULL);

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

  sdmmc_card_t *card;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // Speed set at about .7 max speed that did not through a 109 improper error.
  host.max_freq_khz = 1000;
  
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
    return;
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
  f_res = f_opendir(&baseDir, "");

  if (f_res != F_OK)
  {
    ESP_LOGE(ourTaskName, "Could not open directory (%s)\n", mount_point);
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
      if (file >= MAX_NUM_PAGES)
      {
        continue;
      }

      // Search string for wav type. if type is present, then sub_address will
      // not be null
      char *sub_address =
          strnstr(file_info.fname, ".WAV", MAX_FILE_NAME_LENGTH);

      if (file < MAX_NUM_PAGES && sub_address != NULL)
      {
        strcpy(f_names[file], file_info.fname);
        ESP_LOGI(ourTaskName, "File Name Copy: %s", f_names[file]);
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

  // Sort files by alphanumeric values, IE 0 - 9a - z, ignoring case, for later
  // retrieval qsort(f_names, file, sizeof(f_names[0]), strcasecmp);

  ESP_LOGI(ourTaskName, "File Order: \n");
  for (int i = 0; i < file; ++i)
  {
    ESP_LOGI(ourTaskName, "%s\n", f_names[i]);
  }

  // File opening section:
  // Open file for reading

  // test_file_name length is size of mount point plus maximum size of file name
  char test_file_name[sizeof(mount_point) + 13];
  strcpy(test_file_name, mount_point);
  strncat(test_file_name, "/", 2);
  strncat(test_file_name, f_names[0], sizeof(test_file_name) - 1);

  ESP_LOGI(ourTaskName, "File to open:  %s", test_file_name);

  TinyWav audio_file;

  int err = tinywav_open_read(&audio_file, test_file_name, TW_INTERLEAVED);

  if (err != 0)
  {
    ESP_LOGE(ourTaskName, "Tiny wave could not open file to read.");
    ESP_LOGE(ourTaskName, "Error: %d", err);
    return;
  }

  ESP_LOGI(ourTaskName, "WAV file information: ");
  ESP_LOGI(ourTaskName, "File format (2 for signed, 4 for float) %d",
           audio_file.sampFmt);
  ESP_LOGI(ourTaskName, "Number of audio channels: %d", audio_file.numChannels);
  ESP_LOGI(ourTaskName, "Number of frames in header: %ld",
           audio_file.numFramesInHeader);
  ESP_LOGI(ourTaskName, "Sample rate: %lu", audio_file.h.SampleRate);

  if (audio_file.numChannels != 1 && audio_file.numChannels != 2)
  {
    ESP_LOGI(ourTaskName, "Unsupported number of audio channels. Only Mono or "
                          "Stereo audio is supported.");
    return;
  }

  size_t size_wav_data_bytes = 0;
  switch (audio_file.sampFmt)
  {
  case TW_INT16:
    size_wav_data_bytes = sizeof(uint16_t);
    break;
  case TW_FLOAT32:
    size_wav_data_bytes = sizeof(float);
    break;
  }

  if (size_wav_data_bytes == 0)
  {
    ESP_LOGE(ourTaskName, "Unsupported audio file type");
    return;
  }

  while (1)
  {
    if (xSemaphoreTake(audio_info_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      ESP_LOGI(ourTaskName, "Could not get audio info mutex for writing");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    sample_frequency = audio_file.h.SampleRate;
    bits_sample = audio_file.h.BitsPerSample;
    num_channels = audio_file.numChannels;
    info_changed = true;
    xSemaphoreGive(audio_info_mutex);
    break;
  }

  long int file_loc = ftell(audio_file.f);
  ESP_LOGI(ourTaskName, "Test file read %lx bytes in header", file_loc);

  uint8_t *w_buf = (uint8_t *)calloc(1, BUFF_READ_SIZE);
  assert(w_buf);

  uint8_t *r_buff = (uint8_t *)calloc(1, BUFF_READ_SIZE);
  assert (r_buff);

  uint16_t bytes_in_frame = size_wav_data_bytes * audio_file.numChannels;
  uint16_t frames_in_buff = BUFF_READ_SIZE / bytes_in_frame;

  ESP_LOGI(ourTaskName, "Setup for file reading passed");
  ESP_LOGI(ourTaskName, "Frames in buffer: %d", frames_in_buff);

  int frames = tinywav_read_f(&audio_file, w_buf, r_buff, BUFF_READ_SIZE);

  //ESP_LOGI(ourTaskName, "Frames read: %d", frames);

  if (frames < 0)
  {
    ESP_LOGE(ourTaskName, "Error in reading WAV file");
    return;
  }
  

  while (frames != 0)
  {
    BaseType_t res = xRingbufferSend(audio_handle, &w_buf, BUFF_READ_SIZE, 100);
    if (res != pdTRUE) {
      vTaskDelay(0);
      continue;
    }

    //ESP_LOGI(ourTaskName, "Got to reading buffer");
    frames = tinywav_read_f(&audio_file, w_buf, r_buff, BUFF_READ_SIZE);
    if (frames < 0)
    {
      ESP_LOGE(ourTaskName, "Error in reading WAV file");
      return;
    }
    if (frames == 0)
    {
      tinywav_close_read(&audio_file);
      ESP_LOGI(ourTaskName, "Finished playing track");
      break;
    }
    //ESP_LOGI(ourTaskName, "Frames read: %d", frames);
  }

  while (1)
  {
    vTaskDelay(10000);
  }
}

void write_shared_audio_buffer()
{

  char *ourTaskName = pcTaskGetName(NULL);

  i2s_chan_handle_t tx_handle;

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

  BaseType_t res;

  while (1)
  {
    res = xSemaphoreTake(audio_info_mutex, pdMS_TO_TICKS(10));
    if (res == true && info_changed == true)
    {
      break;
    }
    if (res == true)
    {
      xSemaphoreGive(audio_info_mutex);
    }
    ESP_LOGI(ourTaskName,
             "did not get audio mutex for reading or result was unchanged;");
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  ESP_LOGE(ourTaskName, "Configuring output");
  uint32_t local_freq = sample_frequency;
  i2s_data_bit_width_t local_bit_width = (i2s_data_bit_width_t)bits_sample;
  i2s_slot_mode_t local_slot_mode = (i2s_slot_mode_t)num_channels;
  xSemaphoreGive(audio_info_mutex);

  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = {
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .sample_rate_hz = local_freq
      },
      .slot_cfg =
          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(local_bit_width, local_slot_mode),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = I2S_CLK_PIN,
              .ws = I2S_WS,
              .dout = I2S_DOUT,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,

                  },
          },

  };

  ESP_LOGE(ourTaskName, "Initializing channel");
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));

  ESP_LOGE(ourTaskName, "Creating buffer");

  ESP_LOGE(ourTaskName, "Pre-Loading mem to CPU");

  size_t total_received;
  size_t recieved_data;
  size_t data_read = 0;

  uint8_t* data = NULL;
  while (data == NULL) {
    data = (uint8_t *)xRingbufferReceiveUpTo(audio_handle, &recieved_data, 100, BUFF_READ_SIZE);
  }
  
  total_received = recieved_data;
  
  do {
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx_handle, &data[data_read], total_received - data_read, &recieved_data));
    data_read += recieved_data;
  } while (data_read != recieved_data);

  vRingbufferReturnItem(audio_handle, data);
  
  ESP_LOGE(ourTaskName, "Enabling Channel");
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  while (1)
  {
    data = xRingbufferReceiveUpTo(audio_handle, &total_received, 100, BUFF_READ_SIZE);
    if (data == NULL)
    {
      ESP_LOGE(ourTaskName, "Failed to receive Item");
      continue;
    }
    data_read = 0;
    //ESP_LOGE(ourTaskName, "Writing info to i2S buffer");
    while (data_read != total_received) {
      esp_err_t ret = i2s_channel_write(tx_handle, &data[data_read], total_received - data_read, &recieved_data, 100);
      data_read += recieved_data;
      if (ret == ESP_ERR_TIMEOUT) {
        continue;
      } else if (ret != ESP_OK) {
        vRingbufferReturnItem(audio_handle, data);
        ESP_LOGE(ourTaskName, "Error in I2S interface");
        return;
      }
    }
    vRingbufferReturnItem(audio_handle, data);
    vTaskDelay(1);
  }
}

void on_sent(void * callback_data) {

}