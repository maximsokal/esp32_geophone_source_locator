#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool available;
    bool valid;
    uint32_t age_ms;
    bool stale;
} ina3221_monitor_rail_t;

esp_err_t ina3221_monitor_get_rail(size_t channel_index, ina3221_monitor_rail_t *rail_out);
