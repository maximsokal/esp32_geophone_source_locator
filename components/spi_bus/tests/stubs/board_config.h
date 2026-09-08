#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BOARD_SPI_HOST       SPI2_HOST
#define BOARD_SPI_MOSI_GPIO  GPIO_NUM_11
#define BOARD_SPI_SCLK_GPIO  GPIO_NUM_12
#define BOARD_SPI_MISO_GPIO  GPIO_NUM_13
#define BOARD_TFT_CS_GPIO    GPIO_NUM_14
#define BOARD_SD_CS_GPIO     GPIO_NUM_17
