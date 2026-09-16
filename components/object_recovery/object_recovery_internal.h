#pragma once

#include <stdbool.h>

#include "esp_err.h"


typedef bool (*object_recovery_stop_requested_fn)(void *user_context);


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
);


/* Runtime/background variant: fail closed rather than deleting the object that
 * currently owns persisted ACTIVE authority. The caller must hold the shared
 * storage operation for the whole check/delete window. */
esp_err_t object_recovery_check_one_background(
    const char *object_id,
    bool *removed
);


/* Exposed for deterministic host coverage of the destructive runtime guard. */
esp_err_t object_recovery_background_delete_guard(
    const char *object_id
);


esp_err_t object_recovery_run_cooperative(
    object_recovery_stop_requested_fn stop_requested,
    void *user_context,
    bool yield_between_objects
);
