#pragma once

#include <stdbool.h>

#include "esp_err.h"


typedef bool (*object_recovery_stop_requested_fn)(void *user_context);


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
);


esp_err_t object_recovery_run_cooperative(
    object_recovery_stop_requested_fn stop_requested,
    void *user_context,
    bool yield_between_objects
);
