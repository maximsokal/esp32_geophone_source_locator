#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int spi_host_device_t;
typedef void *spi_device_handle_t;

#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 1
#define SPI_DMA_DISABLED 0
#define SPICOMMON_BUSFLAG_MASTER 1

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int data4_io_num;
    int data5_io_num;
    int data6_io_num;
    int data7_io_num;
    int max_transfer_sz;
    int flags;
    int intr_flags;
} spi_bus_config_t;

typedef struct {
    int command_bits;
    int address_bits;
    int dummy_bits;
    int mode;
    int clock_speed_hz;
    int spics_io_num;
    int queue_size;
} spi_device_interface_config_t;

typedef struct {
    size_t length;
    void *rx_buffer;
} spi_transaction_t;

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
);
esp_err_t spi_bus_add_device(
    spi_host_device_t host_id,
    const spi_device_interface_config_t *config,
    spi_device_handle_t *handle
);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_bus_free(spi_host_device_t host_id);
esp_err_t spi_device_polling_transmit(
    spi_device_handle_t handle,
    spi_transaction_t *transaction
);
int spi_get_freq_limit(bool gpio_is_used, int input_delay_ns);
