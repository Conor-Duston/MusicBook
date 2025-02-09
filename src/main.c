#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "soc/lldesc.h"

#include "esp_log.h"

#include "driver/sdspi_host.h"
#include "driver/i2s_std.h"

#include "tinywav.h"
#include "main.h"

#include "driver/gpio.h"
#include "esp_intr_alloc.h"

#include "file_managment.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

#define SECTION_1_PIN 2
#define SECTION_2_PIN 0
#define SECTION_3_PIN 4
#define SECTION_PIN_SELECT ((1ULL<<SECTION_1_PIN) | (1ULL<<SECTION_2_PIN) | (1ULL<<SECTION_3_PIN))

// I2S driver and output
#define I2S_CLK_PIN 25
#define I2S_DOUT 26
#define I2S_WS 27

#define MIN_DATA_SIZE 96   
#define DATA_MULTIPLIER 32
#define BUFF_SIZE MIN_DATA_SIZE * DATA_MULTIPLIER
#define BUFF_READ_SIZE BUFF_SIZE / 2

TaskHandle_t read_task;

static RingbufHandle_t audio_handle;

volatile uint8_t selection = 0;
volatile IRAM_DATA_ATTR bool selection_changed = false;

static void set_file_read_from(void* arg) { 
  selection_changed = true;
}

void app_main(void)
{  
  char *ourTaskName = pcTaskGetName(NULL);
  
  ESP_LOGI(ourTaskName, "Starting up!\n");
  ESP_LOGI(ourTaskName, "Task name pointer: %p", ourTaskName);

  bool gpio_set = gpio_setup();

  if (!gpio_set) {
    return;
  }

  sdmmc_card_t card;

  bool mounted = mount_fs(&card);
  sort_filenames();

  if (!mounted) {
    return;
  }

  audio_handle = xRingbufferCreate(BUFF_SIZE, RINGBUF_TYPE_BYTEBUF);

  if (audio_handle == NULL)
  {
    ESP_LOGE(ourTaskName, "Could not create ring buffer");
  }

  BaseType_t result =
      xTaskCreatePinnedToCore(read_file_to_shared_buffer, "read_file", 8192 * 8,
                              NULL, 10, &read_task, 1);
  if (result != pdPASS)
  {
    ESP_LOGE(ourTaskName, "Failed to create write task");
    return;
  }

  vTaskDelete(NULL);
}

void read_file_to_shared_buffer()
{
  char *ourTaskName = pcTaskGetName(NULL);

  TinyWav audio_file;

  open_file(0, &audio_file);

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

  long int data_start = ftell(audio_file.f);
  ESP_LOGI(ourTaskName, "Test file read %lx bytes in header", data_start);

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
  
  BaseType_t res = xRingbufferSend(audio_handle, w_buf, BUFF_READ_SIZE, 5);
  if (res != pdTRUE) {
    ESP_LOGE(ourTaskName, "Undetected error in ring buffer creation");
    return;
  }

  i2s_chan_handle_t audio_output;

  bool success = setup_audio_output(&audio_output, audio_file.h.SampleRate, audio_file.h.BitsPerSample, audio_file.h.NumChannels);

  if (!success) {
    return;
  }

  bool playing = true;

  while (1)
  { 
    res = xRingbufferSend(audio_handle, w_buf, BUFF_READ_SIZE, 5);
    if (res != pdTRUE) {
      vTaskDelay(0);
      continue;
    }

    frames = tinywav_read_f(&audio_file, w_buf, r_buff, BUFF_READ_SIZE);
    if (frames < 0)
    {
      ESP_LOGE(ourTaskName, "Error in reading WAV file");
      return;
    }
    if (frames == 0)
    {
      fseek(audio_file.f, data_start, SEEK_SET);
      audio_file.totalFramesReadWritten = 0;
      break;
    }

    if (selection_changed && playing) {
      ESP_LOGI(ourTaskName, "selection has changed");
      selection_changed = false;
      
      ESP_LOGI(ourTaskName, "Doing selection change");
      
      disable_audio_output(&audio_output);
      tinywav_close_read(&audio_file);

      if (selection > NUM_SECTIONS) {
        playing = false;
        break;
      }

      tinywav_close_read(&audio_file);
      open_file(selection, &audio_file);

      frames = tinywav_read_f(&audio_file, w_buf, r_buff, BUFF_READ_SIZE);
      if (frames < 0)
      {
        ESP_LOGE(ourTaskName, "Error in reading WAV file");
        return;
      }

      res = xRingbufferSend(audio_handle, w_buf, BUFF_READ_SIZE, 5);
      if (res != pdTRUE) {
        vTaskDelay(0);
        continue;
      }
      
      reconfigure_audio_output(audio_handle, audio_file.h.SampleRate, audio_file.h.BitsPerSample, audio_file.h.AudioFormat);

    } else if (selection_changed && !playing) {
      if (selection > NUM_SECTIONS) {
        break;
      }

      ESP_LOGI(ourTaskName, "selection has changed");
      selection_changed = false;
      
      ESP_LOGI(ourTaskName, "Doing selection change");

      open_file(selection, &audio_file);

      frames = tinywav_read_f(&audio_file, w_buf, r_buff, BUFF_READ_SIZE);
      if (frames < 0)
      {
        ESP_LOGE(ourTaskName, "Error in reading WAV file");
        return;
      }

      res = xRingbufferSend(audio_handle, w_buf, BUFF_READ_SIZE, 5);
      if (res != pdTRUE) {
        vTaskDelay(0);
        continue;
      }

      reconfigure_audio_output(audio_handle, audio_file.h.SampleRate, audio_file.h.BitsPerSample, audio_file.h.AudioFormat);

    }
    //ESP_LOGI(ourTaskName, "Frames read: %d", frames);
    vTaskDelay(0);
  }
}

static IRAM_ATTR bool data_on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
  size_t recieved_data_size;
  uint8_t* data = xRingbufferReceiveUpToFromISR(audio_handle, &recieved_data_size, event->size);
  if (data == NULL) {
    return false;
  }
  memcpy(event->dma_buf, data, recieved_data_size);
  BaseType_t woke_higher_task;
  vRingbufferReturnItemFromISR(audio_handle, data, &woke_higher_task);
  return woke_higher_task;
}

static void setup_i2s_channel(i2s_chan_handle_t *tx_handle, uint32_t sample_frequency, i2s_data_bit_width_t bits_sample, i2s_slot_mode_t slot_mode) {
  
  i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_AUTO,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 6, 
    .dma_frame_num = 240,
    .auto_clear_after_cb = false,
    .auto_clear_before_cb = false,
    .intr_priority = 0,
  };
  //I2S_CHANNEL_DEFAULT_CONFIG
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx_handle, NULL));

  ESP_LOGI("i2s_channel_setup", "Configuring output");
  
  i2s_std_clk_config_t clock_config = {
    .clk_src = SOC_MOD_CLK_APLL,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .sample_rate_hz = sample_frequency
  };

  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = clock_config,
      .slot_cfg =
          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_sample, slot_mode),
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

  ESP_LOGI("i2s_channel_setup", "Initializing channel");
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx_handle, &tx_std_cfg));

  i2s_event_callbacks_t cbs = {
    .on_recv = NULL,
    .on_recv_q_ovf = NULL,
    .on_sent = data_on_sent,
    .on_send_q_ovf = NULL,
  };

  ESP_ERROR_CHECK(i2s_channel_register_event_callback(*tx_handle, &cbs, NULL));
}

bool setup_audio_output(i2s_chan_handle_t *tx_handle, uint32_t sample_frequency, i2s_data_bit_width_t bits_sample, i2s_slot_mode_t slot_mode)
{
  char *ourTaskName = pcTaskGetName(NULL);

  setup_i2s_channel(tx_handle, sample_frequency, bits_sample, slot_mode);

  ESP_LOGI(ourTaskName, "Pre-Loading mem to CPU");

  size_t total_received;
  size_t data_read = 0;
  uint8_t* data = NULL;
    
  data = xRingbufferReceiveUpTo(audio_handle, &total_received, 0, BUFF_READ_SIZE); 

  ESP_ERROR_CHECK(i2s_channel_preload_data(*tx_handle, data, total_received, &data_read));
  vTaskDelay(0);
  if (data_read != total_received) {
    return false;
  }

  vRingbufferReturnItem(audio_handle, data);

  ESP_LOGI(ourTaskName, "Enabling Channel");
  ESP_ERROR_CHECK(i2s_channel_enable(*tx_handle));
  ESP_LOGI(ourTaskName, "Channel Enabled");
  
  return true;
}

bool disable_audio_output(i2s_chan_handle_t *tx_handle) {
  static const char *ourTaskName = "disable_audio";
  
  size_t total_received;
  uint8_t* data = NULL;

  esp_err_t ret = i2s_channel_disable(*tx_handle);
  
  if (ret != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error disabling channel (%s)", esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(ourTaskName, "Detected change in file name, flushing buffers");
  while (1) {
    data = xRingbufferReceiveUpTo(audio_handle, &total_received, 0, BUFF_READ_SIZE); 
    if (data == NULL) {
      ESP_LOGI(ourTaskName, "Buffer flushed");
      break;
    }
    vRingbufferReturnItem(audio_handle, data);
    vTaskDelay(5);
  }

  return true;
}

bool reconfigure_audio_output(i2s_chan_handle_t *tx_handle, uint32_t sample_frequency, i2s_data_bit_width_t bits_sample, i2s_slot_mode_t slot_mode) {
  static const char *ourTaskName = "reconfigure_audio";

  size_t total_received;
  size_t data_read = 0;
  uint8_t* data = NULL;

  i2s_std_clk_config_t clock_config = {
    .clk_src = SOC_MOD_CLK_APLL,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .sample_rate_hz = sample_frequency
  };

  esp_err_t ret = i2s_channel_reconfig_std_clock(*tx_handle, &clock_config);
  if (ret != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error reconfiguring channel (%s)", esp_err_to_name(ret));
    return false;
  }

  i2s_std_slot_config_t slot_cnfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_sample, slot_mode);
  i2s_channel_reconfig_std_slot(*tx_handle, &slot_cnfg);
  
  data = xRingbufferReceiveUpTo(audio_handle, &total_received, 0, BUFF_READ_SIZE); 

  ESP_ERROR_CHECK(i2s_channel_preload_data(*tx_handle, data, total_received, &data_read));
  vTaskDelay(0);
  if (data_read != total_received) {
    return false;
  }

  vRingbufferReturnItem(audio_handle, data);
  
  ESP_ERROR_CHECK(i2s_channel_enable(*tx_handle));

  return true;
}

bool gpio_setup() {
  const static char* ourTaskName = "gpio_setup";

  gpio_config_t io_conf = {};

  io_conf.intr_type = GPIO_INTR_ANYEDGE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
  io_conf.intr_type = GPIO_INTR_ANYEDGE;
  io_conf.pin_bit_mask = SECTION_PIN_SELECT;

  esp_err_t err = gpio_config(&io_conf);

  if (err != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error configuring IO pins (%s)", esp_err_to_name(err));
    return false;
  }
  
  err = gpio_install_isr_service(ESP_INTR_FLAG_EDGE | ESP_INTR_FLAG_LEVEL3);
  
  if (err != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error installing isr service (%s)", esp_err_to_name(err));
    return false;
  }

  err = gpio_isr_handler_add(SECTION_1_PIN, set_file_read_from, (void*) SECTION_1_PIN);

  if (err != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error adding interrupt handler to pin %d, (%s)", SECTION_1_PIN, esp_err_to_name(err));
    return false;
  }

  err = gpio_isr_handler_add(SECTION_2_PIN, set_file_read_from, (void*) SECTION_2_PIN);

  if (err != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error adding interrupt handler to pin %d, (%s)", SECTION_2_PIN, esp_err_to_name(err));
    return false;
  }
  
  err = gpio_isr_handler_add(SECTION_3_PIN, set_file_read_from, (void*) SECTION_3_PIN);

  if (err != ESP_OK) {
    ESP_LOGE(ourTaskName, "Error adding interrupt handler to pin %d, (%s)", SECTION_3_PIN, esp_err_to_name(err));
    return false;
  }

  return true;
}