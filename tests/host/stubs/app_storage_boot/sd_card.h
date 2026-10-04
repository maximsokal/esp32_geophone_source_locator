#pragma once

#include "esp_err.h"

typedef struct {
    int marker;
} sdmmc_card_t;

typedef enum {
    SD_CARD_MOUNT_STAGE_NONE = 0,
    SD_CARD_MOUNT_STAGE_HOST_INIT,
    SD_CARD_MOUNT_STAGE_CARD_INIT,
    SD_CARD_MOUNT_STAGE_FS_MOUNT,
} sd_card_mount_stage_t;

typedef enum {
    SD_CARD_HEALTH_UNKNOWN = 0,
    SD_CARD_HEALTH_READY,
    SD_CARD_HEALTH_CARD_UNAVAILABLE,
    SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE,
} sd_card_health_t;

typedef struct {
    sd_card_mount_stage_t stage;
    esp_err_t err;
    int fresult;
    sd_card_health_t health;
} sd_card_mount_report_t;

esp_err_t sd_card_mount(void);
esp_err_t sd_card_mount_recovery(sd_card_mount_report_t *report);
sdmmc_card_t *sd_card_get_card_handle(void);
esp_err_t sd_card_unmount(void);
