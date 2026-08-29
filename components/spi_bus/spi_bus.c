#include "spi_bus.h"

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"


static const char *TAG =
    "SPI_BUS";


static esp_err_t app_spi_bus_set_level_checked(
    gpio_num_t pin,
    uint32_t level,
    const char *name
)
{
    esp_err_t err =
        gpio_set_level(
            pin,
            level
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set %s=%u: %s",
            name,
            (unsigned)level,
            esp_err_to_name(err)
        );
    }

    return err;
}


/*
 * Put both SPI2 devices into a deterministic deselected state
 * before the bus is initialized and before the first transfer.
 *
 * AD7606 no longer shares this bus in Stage 11B. Its GPIO and
 * SPI3 lifecycle are owned by the acquisition task on Core 1.
 */
static esp_err_t app_spi_bus_prepare_device_idle_levels(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask =
            (1ULL << BOARD_TFT_CS_GPIO) |
            (1ULL << BOARD_SD_CS_GPIO),

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &output_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure SPI2 idle pins: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        app_spi_bus_set_level_checked(
            BOARD_TFT_CS_GPIO,
            1,
            "TFT_CS"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        app_spi_bus_set_level_checked(
            BOARD_SD_CS_GPIO,
            1,
            "SD_CS"
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Safe SPI2 boot levels: TFT_CS=1 SD_CS=1"
    );

    return ESP_OK;
}


esp_err_t app_spi_bus_init(void)
{
    esp_err_t err =
        app_spi_bus_prepare_device_idle_levels();

    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num =
            BOARD_SPI_MOSI_GPIO,

        .miso_io_num =
            BOARD_SPI_MISO_GPIO,

        .sclk_io_num =
            BOARD_SPI_SCLK_GPIO,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .data4_io_num =
            -1,

        .data5_io_num =
            -1,

        .data6_io_num =
            -1,

        .data7_io_num =
            -1,

        /*
         * Buffer is large enough to transfer 40 RGB565 rows.
         */
        .max_transfer_sz =
            320 * 40 * sizeof(uint16_t),

        .flags =
            SPICOMMON_BUSFLAG_MASTER,

        .intr_flags =
            0,
    };

    ESP_LOGI(
        TAG,
        "Initializing SPI2: MOSI=%d MISO=%d SCLK=%d",
        BOARD_SPI_MOSI_GPIO,
        BOARD_SPI_MISO_GPIO,
        BOARD_SPI_SCLK_GPIO
    );

    err =
        spi_bus_initialize(
            BOARD_SPI_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        );

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "SPI2 bus is already initialized"
        );

        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI2 bus initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "SPI2 initialized successfully"
    );

    return ESP_OK;
}


spi_host_device_t app_spi_bus_get_host(void)
{
    return BOARD_SPI_HOST;
}
