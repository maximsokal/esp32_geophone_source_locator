#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"


/*
 * Install the process-wide GPIO ISR service with IRAM support. Call this
 * before registering any GPIO ISR. ESP-IDF cannot report the flags of a
 * service installed elsewhere, so ESP_ERR_INVALID_STATE is accepted under
 * the source-level sole-owner rule: no other code may install this service.
 *
 * The service, and so every GPIO handler, runs on BOARD_GPIO_ISR_CORE whatever
 * core the caller is on. The only GPIO interrupts in this firmware are the
 * AD7606 BUSY edges (acquisition and presence monitor); they belong on the
 * realtime core next to the acquisition task, not on the service core, where
 * Wi-Fi, LVGL and storage critical sections would delay every sample wake-up.
 * Must be called from a task with the scheduler running.
 */
#define BOARD_GPIO_ISR_CORE 1

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

// Main-enclosure status LEDs; actual soldered wiring (2026-10-25).
#define BOARD_STATUS_LED_RED_GPIO     GPIO_NUM_4
#define BOARD_STATUS_LED_YELLOW_GPIO  GPIO_NUM_5
#define BOARD_STATUS_LED_GREEN_GPIO   GPIO_NUM_6

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
