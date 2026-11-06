#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    MEASUREMENT_SERVICE_LIVE = 0,
    MEASUREMENT_SERVICE_STARTING,
    MEASUREMENT_SERVICE_RECORDING,
    MEASUREMENT_SERVICE_STOPPING,
    MEASUREMENT_SERVICE_REVIEW,
    MEASUREMENT_SERVICE_DELETING,
    MEASUREMENT_SERVICE_ERROR
} measurement_service_state_t;

typedef struct {
    bool active_object_present;
    measurement_service_state_t state;
    char object_id[16];
    char session_id[16];
    esp_err_t last_error;
    esp_err_t stop_stage_error;
} measurement_service_snapshot_t;

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot);
esp_err_t measurement_service_start_async(void);
esp_err_t measurement_service_stop_async(void);
esp_err_t measurement_service_delete_async(void);
