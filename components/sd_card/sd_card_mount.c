#include "sd_card.h"

#include <stdio.h>

#include "board_config.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sd_card_internal.h"


static const char *TAG =
    "SD_CARD";


static void sd_card_log_spi_levels(
    const char *stage
)
{
    ESP_LOGW(
        TAG,
        "%s SPI2 levels: SD_CS=%d TFT_CS=%d MISO=%d MOSI=%d SCLK=%d",
        stage,
        gpio_get_level(BOARD_SD_CS_GPIO),
        gpio_get_level(BOARD_TFT_CS_GPIO),
        gpio_get_level(BOARD_SPI_MISO_GPIO),
        gpio_get_level(BOARD_SPI_MOSI_GPIO),
        gpio_get_level(BOARD_SPI_SCLK_GPIO)
    );
}


static void sd_card_log_mount_failure(
    esp_err_t err
)
{
    ESP_LOGE(
        TAG,
        "SD mount failed: %s",
        esp_err_to_name(err)
    );

    sd_card_log_spi_levels(
        "mount failure"
    );

    if (err == ESP_ERR_TIMEOUT) {
        /*
         * sdmmc_card_init() performs card discovery at the protocol probing
         * clock before switching to host.max_freq_khz. A timeout here is
         * therefore a card-response / wiring / power / bus-state problem, not
         * a reason to silently lower the normal 20 MHz runtime clock.
         */
        ESP_LOGE(
            TAG,
            "Card did not answer during SD protocol initialization; check 5V adapter supply, 3.3V regulator output, CS/MISO continuity and required SD pull-ups"
        );
    }
}


esp_err_t sd_card_mount(void)
{
    if (
        sd_card_internal_card() !=
        NULL
    ) {
        ESP_LOGW(
            TAG,
            "SD card is already mounted"
        );

        return ESP_OK;
    }

    esp_err_t err =
        sd_card_internal_prepare_shared_spi();

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Follow ESP-IDF's SDSPI production sequence for a shared bus:
     *   1. SPI2 has already been initialized by app_spi_bus_init().
     *   2. Every other device CS (TFT) is held HIGH.
     *   3. The SD card is mounted before the TFT performs any SPI traffic.
     *
     * SDSPI_HOST_DEFAULT() uses the SD protocol probing clock during card
     * discovery and then switches to the configured normal runtime ceiling.
     * For SDSPI that standard ceiling is 20 MHz.
     */
    sdmmc_host_t host =
        SDSPI_HOST_DEFAULT();

    host.slot =
        BOARD_SPI_HOST;

    host.max_freq_khz =
        SD_CARD_SPI_FREQ_KHZ;

    /* Match the current ESP-IDF SDSPI example for unaligned multi-block I/O. */
    host.unaligned_multi_block_rw_max_chunk_size =
        8;

    sdspi_device_config_t slot_config =
        SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.host_id =
        BOARD_SPI_HOST;

    slot_config.gpio_cs =
        BOARD_SD_CS_GPIO;

    const esp_vfs_fat_mount_config_t
        mount_config = {
            .format_if_mount_failed =
                false,

            .max_files =
                5,

            .allocation_unit_size =
                32 * 1024,

            .disk_status_check_enable =
                false,
        };

    ESP_LOGI(
        TAG,
        "Initializing SD over shared SPI2: protocol probe -> runtime ceiling %.2f MHz",
        (double)host.max_freq_khz / 1000.0
    );

    sdmmc_card_t *card =
        NULL;

    err =
        esp_vfs_fat_sdspi_mount(
            sd_card_internal_mount_point(),
            &host,
            &slot_config,
            &mount_config,
            &card
        );

    if (err != ESP_OK) {
        sd_card_log_mount_failure(
            err
        );

        return err;
    }

    sd_card_internal_set_card(
        card
    );

    ESP_LOGI(
        TAG,
        "SD card mounted: requested=%d kHz card_max=%u kHz real=%d kHz",
        host.max_freq_khz,
        (unsigned)card->max_freq_khz,
        card->real_freq_khz
    );

    if (
        card->real_freq_khz > 0 &&
        card->real_freq_khz <
            (SD_CARD_SPI_FREQ_KHZ * 9) / 10
    ) {
        ESP_LOGW(
            TAG,
            "SD runtime clock is below the expected 20 MHz class: %d kHz",
            card->real_freq_khz
        );
    }

    sdmmc_card_print_info(
        stdout,
        card
    );

    return ESP_OK;
}
