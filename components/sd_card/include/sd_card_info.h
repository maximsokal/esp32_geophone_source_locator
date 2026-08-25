#pragma once

#include <stdint.h>

#include "esp_err.h"


#define SD_CARD_NAME_SIZE  8
#define SD_CARD_TYPE_SIZE  24


typedef struct
{
    char name[
        SD_CARD_NAME_SIZE
    ];

    char type[
        SD_CARD_TYPE_SIZE
    ];

    uint64_t capacity_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;

    uint32_t speed_khz;

} sd_card_info_t;


esp_err_t sd_card_get_info(
    sd_card_info_t *info
);
