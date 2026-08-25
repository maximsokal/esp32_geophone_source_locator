#include "sd_card.h"

#include <stdio.h>

#include "board_config.h"

#include "driver/sdspi_host.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sd_card_internal.h"


static const char *TAG =
    "SD_CARD";


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

    ESP_LOGI(
        TAG,
        "Mounting SD card at 2.00 MHz"
    );

    sdmmc_host_t host =
        SDSPI_HOST_DEFAULT();

    host.slot =
        BOARD_SPI_HOST;

    host.max_freq_khz =
        SD_CARD_SPI_FREQ_KHZ;

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
        ESP_LOGE(
            TAG,
            "SD mount failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    sd_card_internal_set_card(
        card
    );

    ESP_LOGI(
        TAG,
        "SD card mounted successfully"
    );

    sdmmc_card_print_info(
        stdout,
        card
    );

    return ESP_OK;
}
