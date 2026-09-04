#include "sd_card.h"

#include <stdio.h>
#include <stdint.h>

#include "board_config.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_card_internal.h"


#define SD_CARD_MOUNT_ATTEMPTS       3U
#define SD_CARD_POWER_STABILIZE_MS   120U
#define SD_CARD_RETRY_DELAY_MS       180U


static const char *TAG =
    "SD_CARD";


static uint32_t sd_card_mount_frequency_khz(
    uint32_t attempt
)
{
    static const uint32_t frequencies_khz[SD_CARD_MOUNT_ATTEMPTS] = {
        SD_CARD_SPI_FREQ_KHZ,
        1000U,
        400U,
    };

    return frequencies_khz[attempt];
}


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
     * Some inexpensive SPI microSD adapter boards need a short settling
     * interval after the ESP32 and the adapter power up together. The old
     * boot path attempted CMD0 immediately after configuring the shared bus;
     * a single transient timeout therefore made storage permanently fatal for
     * that boot. Keep TFT deselected, give the card time to settle, then make
     * bounded retries at progressively more conservative SPI clocks.
     */
    vTaskDelay(
        pdMS_TO_TICKS(
            SD_CARD_POWER_STABILIZE_MS
        )
    );

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

    esp_err_t last_err =
        ESP_FAIL;

    for (
        uint32_t attempt = 0;
        attempt < SD_CARD_MOUNT_ATTEMPTS;
        ++attempt
    ) {
        const uint32_t frequency_khz =
            sd_card_mount_frequency_khz(
                attempt
            );

        ESP_LOGI(
            TAG,
            "Mounting SD card: attempt %u/%u at %.2f MHz",
            (unsigned)(attempt + 1U),
            (unsigned)SD_CARD_MOUNT_ATTEMPTS,
            (double)frequency_khz / 1000.0
        );

        sdmmc_host_t host =
            SDSPI_HOST_DEFAULT();

        host.slot =
            BOARD_SPI_HOST;

        host.max_freq_khz =
            (int)frequency_khz;

        sdspi_device_config_t slot_config =
            SDSPI_DEVICE_CONFIG_DEFAULT();

        slot_config.host_id =
            BOARD_SPI_HOST;

        slot_config.gpio_cs =
            BOARD_SD_CS_GPIO;

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

        if (err == ESP_OK) {
            sd_card_internal_set_card(
                card
            );

            ESP_LOGI(
                TAG,
                "SD card mounted successfully on attempt %u at %.2f MHz",
                (unsigned)(attempt + 1U),
                (double)frequency_khz / 1000.0
            );

            sdmmc_card_print_info(
                stdout,
                card
            );

            return ESP_OK;
        }

        last_err =
            err;

        ESP_LOGE(
            TAG,
            "SD mount attempt %u/%u failed: %s",
            (unsigned)(attempt + 1U),
            (unsigned)SD_CARD_MOUNT_ATTEMPTS,
            esp_err_to_name(err)
        );

        sd_card_log_spi_levels(
            "after mount failure"
        );

        /* Leave both devices explicitly deselected before the next CMD0. */
        (void)gpio_set_level(
            BOARD_SD_CS_GPIO,
            1
        );

        (void)gpio_set_level(
            BOARD_TFT_CS_GPIO,
            1
        );

        if (
            attempt + 1U <
            SD_CARD_MOUNT_ATTEMPTS
        ) {
            vTaskDelay(
                pdMS_TO_TICKS(
                    SD_CARD_RETRY_DELAY_MS
                )
            );
        }
    }

    ESP_LOGE(
        TAG,
        "SD mount failed after %u attempts: %s",
        (unsigned)SD_CARD_MOUNT_ATTEMPTS,
        esp_err_to_name(last_err)
    );

    return last_err;
}
