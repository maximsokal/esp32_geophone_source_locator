#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"


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
    int fresult; /* -1 when FatFs did not provide a result. */
    sd_card_health_t health;
} sd_card_mount_report_t;


esp_err_t sd_card_mount(void);


/* Recovery mount preserves an initialized card handle on filesystem failure. */
esp_err_t sd_card_mount_recovery(
    sd_card_mount_report_t *report
);


bool sd_card_is_mounted(void);


const char *sd_card_mount_point(void);


esp_err_t sd_card_unmount(void);


/* Borrowed handle; valid until sd_card_unmount() or a failed card init. */
sdmmc_card_t *sd_card_get_card_handle(void);

#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
/* TEST ONLY (ESP-EMU): transfers that found no DMA bounce buffer, as the real SDSPI driver would have failed on. */
unsigned sd_card_test_flash_bounce_failures(void);
/* TEST ONLY (ESP-EMU): write calls charged the board's SD write latency and the milliseconds charged in total. */
void sd_card_test_flash_write_model(unsigned *calls, unsigned long long *delay_ms);
#endif
