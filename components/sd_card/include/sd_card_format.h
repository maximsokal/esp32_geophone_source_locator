#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"


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


/*
 * Start explicit FAT32 formatting. confirmed must be true; no caller can
 * start destructive formatting through this API without an affirmative UI
 * decision. The card is borrowed from the SD-card recovery mount.
 */
esp_err_t sd_card_format_fat32_task_start(
    sdmmc_card_t *card,
    bool confirmed
);

esp_err_t sd_card_format_fat32_task_get_status(
    sd_card_format_status_t *out_status
);

/* Request cooperative cancellation at the next bounded disk-I/O checkpoint. */
esp_err_t sd_card_format_fat32_task_stop(void);
