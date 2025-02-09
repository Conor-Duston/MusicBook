#pragma once

#include "driver/sdspi_host.h"
#include "tinywav.h"

// SD card file system definitions
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define MOUNT_POINT "/sdc"

#define NUM_SECTIONS 3
#define MAX_FILE_NAME_LENGTH 13

bool mount_fs(sdmmc_card_t *card);

void sort_filenames();

int open_file(const int index, TinyWav *file_opened);