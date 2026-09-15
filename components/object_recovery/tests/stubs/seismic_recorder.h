#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


typedef bool (*seismic_recorder_recovery_stop_requested_fn)(
    void *user_context
);


static inline esp_err_t seismic_recorder_recover_object_sessions(
    const char *object_id,
    seismic_recorder_recovery_stop_requested_fn stop_requested,
    void *user_context,
    size_t *recovered_count
)
{
    (void)object_id;
    if (recovered_count != NULL) {
        *recovered_count = 0U;
    }
    if (stop_requested != NULL) {
        (void)stop_requested(user_context);
    }
    return ESP_OK;
}
