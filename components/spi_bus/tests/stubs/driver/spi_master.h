#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef int spi_host_device_t;

#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO (-1)
#define SPICOMMON_BUSFLAG_MASTER (1U << 0)

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
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
);
