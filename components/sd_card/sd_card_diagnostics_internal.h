#pragma once

#include <stddef.h>

#include "esp_err.h"


esp_err_t sd_card_diagnostics_write_file(
    const char *path,
    const char *text
);


esp_err_t sd_card_diagnostics_read_file(
    const char *path,
    char *buffer,
    size_t buffer_size
);
