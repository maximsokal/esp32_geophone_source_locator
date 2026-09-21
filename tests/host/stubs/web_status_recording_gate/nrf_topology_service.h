#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    size_t assigned_count;
    size_t accepted_count;
} nrf_topology_leveling_summary_t;

esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary
);
