#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ui_boot_init(void);
void ui_boot_show_error(const char *message);
void ui_boot_show_storage_recovery(
    const char *reason,
    const char *technical_code,
    const char *warning,
    const char *primary_hint,
    const char *retry_hint,
    const char *cancel_hint
);
void ui_boot_show_format_progress(unsigned percent);
void ui_boot_show_recovery_result(
    bool success,
    bool formatted,
    const char *error_name
);
void ui_shutdown(void);
