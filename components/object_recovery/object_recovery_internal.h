#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
);
