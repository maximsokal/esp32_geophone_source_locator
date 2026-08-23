#include "spi_bus.h"

#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "SPI_BUS";

esp_err_t app_spi_bus_init(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_SPI_MISO_GPIO,
        .sclk_io_num = BOARD_SPI_SCLK_GPIO,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,

        // Буфер достаточен для отправки 40 строк RGB565 за транзакцию.
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t),

        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };

    ESP_LOGI(
        TAG,
        "Initializing SPI2: MOSI=%d MISO=%d SCLK=%d",
        BOARD_SPI_MOSI_GPIO,
        BOARD_SPI_MISO_GPIO,
        BOARD_SPI_SCLK_GPIO
    );

    esp_err_t err = spi_bus_initialize(
        BOARD_SPI_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO
    );

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus is already initialized");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI bus initialization failed: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    ESP_LOGI(TAG, "SPI2 initialized successfully");

    return ESP_OK;
}

spi_host_device_t app_spi_bus_get_host(void)
{
    return BOARD_SPI_HOST;
}