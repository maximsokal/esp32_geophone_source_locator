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

/* Boot-only variant: runtime leases cannot survive a reboot, but persisted ACTIVE still protects the tree. */
esp_err_t object_recovery_check_one_boot(
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


/*
 * MAX-167: migrate one object's legacy object-level 3D scan (if any) onto
 * its newest recorded session. See object_recovery_legacy_scan.c for the
 * full policy (no session -> left in place; destination already has a scan
 * -> left in place; always returns ESP_OK, since a stuck legacy file for one
 * object must never block recovery for every other object).
 */
esp_err_t object_recovery_migrate_legacy_scan(
    const char *object_id
);
