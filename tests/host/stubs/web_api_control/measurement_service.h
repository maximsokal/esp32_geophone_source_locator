#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool busy;
    int state;
} measurement_service_snapshot_t;

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot);
bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot
);
bool measurement_service_snapshot_blocks_acquisition_stop(
    const measurement_service_snapshot_t *snapshot
);
esp_err_t measurement_service_start_object_async(const char *object_id);
esp_err_t measurement_service_stop_async(void);
