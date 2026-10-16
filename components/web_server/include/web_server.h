#pragma once

#include <stdint.h>

#include "esp_err.h"


esp_err_t web_server_prepare_webui(void);

/*
 * Keep the archive-wide object/session status refresh off the interactive boot
 * path. Production startup closes this gate before web_server_start(), then
 * re-enables it after background object recovery completes.
 */
void web_server_defer_status_cache_refresh(void);
esp_err_t web_server_enable_status_cache_refresh(void);

esp_err_t web_server_start(void);

esp_err_t web_server_stop(void);

void web_status_json_set_reset_diagnostics(
    const char *reason_name,
    int reason_code,
    uint32_t boot_id,
    uint64_t uptime_ms
);
