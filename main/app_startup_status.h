#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_startup_reset_reason.h"

typedef struct
{
    bool valid;
    esp_reset_reason_t reason;
    char reason_name[24];
    uint32_t boot_id;
    uint64_t uptime_ms;
} app_startup_reset_diagnostics_t;

void app_startup_log_success(void);
const app_startup_reset_diagnostics_t *app_startup_get_reset_diagnostics(void);
