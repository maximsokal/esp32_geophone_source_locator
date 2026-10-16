#pragma once

#include <stdint.h>

#include "esp_err.h"

void web_status_json_set_reset_diagnostics(
    const char *reason_name,
    int reason_code,
    uint32_t boot_id,
    uint64_t uptime_ms
);
