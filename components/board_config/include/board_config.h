#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"


/*
 * Install the process-wide GPIO ISR service with IRAM support. Call this
 * before registering any GPIO ISR. ESP-IDF cannot report the flags of a
 * service installed elsewhere, so ESP_ERR_INVALID_STATE is accepted under
 * the source-level sole-owner rule: no other code may install this service.
 */
esp_err_t board_gpio_isr_service_install(void);


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

// Sensor I2C bus. sensor_bus owns I2C0 and shares borrowed device handles.
#define BOARD_SENSOR_I2C_SDA_GPIO     GPIO_NUM_9
#define BOARD_SENSOR_I2C_SCL_GPIO     GPIO_NUM_10
#define BOARD_INA3221_I2C_ADDRESS     0x40

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

// GPIO1 remains physically occupied by the existing divider wiring and must
// not be reused by AD7606.
