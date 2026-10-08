#pragma once

#include "esp_err.h"

typedef enum {
    SD_CARD_PROBE_STAGE_NONE = 0,
    SD_CARD_PROBE_STAGE_CREATE,
    SD_CARD_PROBE_STAGE_WRITE,
    SD_CARD_PROBE_STAGE_FLUSH,
    SD_CARD_PROBE_STAGE_CLOSE,
    SD_CARD_PROBE_STAGE_READ,
    SD_CARD_PROBE_STAGE_COMPARE,
    SD_CARD_PROBE_STAGE_REMOVE,
    SD_CARD_PROBE_STAGE_COMPLETE,
    SD_CARD_PROBE_STAGE_MOUNT_CHECK,
} sd_card_probe_stage_t;

typedef struct {
    sd_card_probe_stage_t stage;
    esp_err_t err;
    int sys_errno;
} sd_card_probe_result_t;

esp_err_t sd_card_diagnostics_run_write_read_test(void);
sd_card_probe_result_t sd_card_write_readback_probe(void);
const char *sd_card_probe_stage_name(sd_card_probe_stage_t stage);
