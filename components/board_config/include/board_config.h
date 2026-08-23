#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// Shared SPI bus for TFT + microSD
#define BOARD_SPI_HOST          SPI2_HOST

#define BOARD_SPI_MOSI_GPIO     GPIO_NUM_11
#define BOARD_SPI_SCLK_GPIO     GPIO_NUM_12
#define BOARD_SPI_MISO_GPIO     GPIO_NUM_13

// TFT
#define BOARD_TFT_CS_GPIO       GPIO_NUM_14
#define BOARD_TFT_DC_GPIO       GPIO_NUM_15
#define BOARD_TFT_RESET_GPIO    GPIO_NUM_16

// microSD (reserved, not used yet)
#define BOARD_SD_CS_GPIO        GPIO_NUM_17

// Pololu power control
#define BOARD_POWER_OFF_GPIO    GPIO_NUM_18

// External power button sense
#define BOARD_POWER_BUTTON_GPIO GPIO_NUM_21

// 5-button analog keypad KS0466
#define BOARD_KEYPAD_GPIO       GPIO_NUM_8