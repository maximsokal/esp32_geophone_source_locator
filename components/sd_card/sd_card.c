#include "sd_card.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sd_card_internal.h"


static const char *TAG =
    "SD_CARD";


bool sd_card_is_mounted(void)
{
    if (
        sd_card_internal_card() ==
        NULL
    ) {
        return false;
    }

    uint64_t total_bytes =
        0;

    uint64_t free_bytes =
        0;

    return
        esp_vfs_fat_info(
            sd_card_internal_mount_point(),
            &total_bytes,
            &free_bytes
        ) ==
        ESP_OK;
}


const char *sd_card_mount_point(void)
{
    return
        sd_card_internal_mount_point();
}


esp_err_t sd_card_unmount(void)
{
    sdmmc_card_t *card =
        sd_card_internal_card();

    if (card == NULL) {
        return ESP_OK;
    }

    esp_err_t err =
        esp_vfs_fat_sdcard_unmount(
            sd_card_internal_mount_point(),
            card
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "SD unmount returned: %s",
            esp_err_to_name(err)
        );

        /*
         * Keep the card handle published so state still matches
         * reality and a later recovery/unmount attempt is possible.
         */
        return err;
    }

    sd_card_internal_set_card(
        NULL
    );

    ESP_LOGI(
        TAG,
        "SD card unmounted"
    );

    return ESP_OK;
}
