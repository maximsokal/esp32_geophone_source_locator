#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_card.h"

typedef enum {
    SD_CARD_FORMAT_STATE_IDLE = 0,
    SD_CARD_FORMAT_STATE_RUNNING,
    SD_CARD_FORMAT_STATE_DONE,
    SD_CARD_FORMAT_STATE_FAILED,
} sd_card_format_state_t;

typedef struct {
    sd_card_format_state_t state;
    uint8_t percent;
    esp_err_t err;
    int fresult;
} sd_card_format_status_t;

esp_err_t sd_card_format_fat32_task_start(sdmmc_card_t *card, bool confirmed);
esp_err_t sd_card_format_fat32_task_get_status(sd_card_format_status_t *out_status);
esp_err_t sd_card_format_fat32_task_stop(void);
