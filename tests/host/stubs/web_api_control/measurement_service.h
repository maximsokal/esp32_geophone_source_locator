#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool busy;
    int state;
} measurement_service_snapshot_t;

enum {
    MEASUREMENT_SERVICE_LIVE = 0,
    MEASUREMENT_SERVICE_STARTING,
    MEASUREMENT_SERVICE_RECORDING,
    MEASUREMENT_SERVICE_STOPPING,
    MEASUREMENT_SERVICE_REVIEW,
    MEASUREMENT_SERVICE_DELETING,
    MEASUREMENT_SERVICE_ERROR,
};

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot);
bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot
);
bool measurement_service_snapshot_blocks_acquisition_stop(
    const measurement_service_snapshot_t *snapshot
);
esp_err_t measurement_service_start_object_async(const char *object_id);
esp_err_t measurement_service_stop_async(void);
