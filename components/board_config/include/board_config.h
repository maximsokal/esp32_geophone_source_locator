#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"


// Shared SPI2 bus for TFT + microSD only.
#define BOARD_SPI_HOST          SPI2_HOST

#define BOARD_SPI_MOSI_GPIO     GPIO_NUM_11
#define BOARD_SPI_SCLK_GPIO     GPIO_NUM_12
#define BOARD_SPI_MISO_GPIO     GPIO_NUM_13

// TFT
#define BOARD_TFT_CS_GPIO       GPIO_NUM_14
#define BOARD_TFT_DC_GPIO       GPIO_NUM_15
#define BOARD_TFT_RESET_GPIO    GPIO_NUM_16

// microSD
#define BOARD_SD_CS_GPIO        GPIO_NUM_17

// Dedicated AD7606 acquisition bus on SPI3.
// MOSI is intentionally unused: AD7606 serial mode is read-only.
#define BOARD_AD7606_SPI_HOST       SPI3_HOST
#define BOARD_AD7606_SPI_MISO_GPIO  GPIO_NUM_39
#define BOARD_AD7606_SPI_SCLK_GPIO  GPIO_NUM_40

#define BOARD_AD7606_CS_GPIO        GPIO_NUM_41
#define BOARD_AD7606_CONVST_GPIO    GPIO_NUM_42
#define BOARD_AD7606_BUSY_GPIO      GPIO_NUM_47
#define BOARD_AD7606_RESET_GPIO     GPIO_NUM_2

// Pololu power control
#define BOARD_POWER_OFF_GPIO    GPIO_NUM_18

// External power button sense
#define BOARD_POWER_BUTTON_GPIO GPIO_NUM_21

// 5-button analog keypad KS0466
#define BOARD_KEYPAD_GPIO       GPIO_NUM_8

// Temporary direct geophone bench input / existing divider wiring.
// GPIO1 is reserved here and must not be reused by AD7606.
#define BOARD_GEOPHONE_BENCH_GPIO GPIO_NUM_1
