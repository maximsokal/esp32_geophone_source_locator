#include "sd_card_info.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_vfs_fat.h"

#include "sd_card_internal.h"


static void sd_card_make_uppercase(
    char *text
)
{
    while (
        *text !=
        '\0'
    ) {
        *text =
            (char)toupper(
                (unsigned char)*text
            );

        text++;
    }
}


static const char *sd_card_type_name(
    const sdmmc_card_t *card
)
{
    if (card->is_sdio) {
        return "SDIO";
    }

    if (card->is_mmc) {
        return "MMC";
    }

    uint64_t capacity =
        (uint64_t)card->csd.capacity *
        (uint64_t)card->csd.sector_size;

    if (
        capacity >
        2ULL *
        1024ULL *
        1024ULL *
        1024ULL
    ) {
        return "SDHC/SDXC";
    }

    return "SDSC";
}


esp_err_t sd_card_get_info(
    sd_card_info_t *info
)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_card_t *card =
        sd_card_internal_card();

    if (card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(
        info,
        0,
        sizeof(*info)
    );

    snprintf(
        info->name,
        sizeof(info->name),
        "%s",
        card->cid.name
    );

    sd_card_make_uppercase(
        info->name
    );

    snprintf(
        info->type,
        sizeof(info->type),
        "%s",
        sd_card_type_name(
            card
        )
    );

    info->capacity_bytes =
        (uint64_t)card->csd.capacity *
        (uint64_t)card->csd.sector_size;

    info->speed_khz =
        card->real_freq_khz > 0
            ? (uint32_t)
                card->real_freq_khz
            : card->max_freq_khz;

    return
        esp_vfs_fat_info(
            sd_card_internal_mount_point(),
            &info->total_bytes,
            &info->free_bytes
        );
}
