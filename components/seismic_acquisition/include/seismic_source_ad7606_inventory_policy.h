#pragma once

#include "esp_err.h"
#include "hw_inventory.h"


typedef enum {
    SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED = 0,
    SEISMIC_SOURCE_AD7606_START_FAILURE_RAIL_GATE,
    SEISMIC_SOURCE_AD7606_START_FAILURE_BUSY_TIMEOUT,
} seismic_source_ad7606_start_failure_stage_t;

typedef struct {
    hw_module_state_t state;
    char reason[HW_INVENTORY_REASON_LEN];
} seismic_source_ad7606_start_failure_t;

seismic_source_ad7606_start_failure_t seismic_source_ad7606_classify_start_failure(
    esp_err_t err,
    seismic_source_ad7606_start_failure_stage_t stage
);
