#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"


typedef void (*object_recovery_background_complete_fn)(
    esp_err_t result,
    void *user_context
);

#define OBJECT_RECOVERY_DELETE_ATTEMPTS_SIZE 160U

typedef struct
{
    bool valid;
    char object_id[16];
    char reason[32];
    char when[8];
    uint64_t uptime_ms;
    uint32_t boot_id;
    uint32_t sessions;
    uint32_t artifacts;
    uint32_t localization_runs;
    uint64_t bytes;
    char attempts[OBJECT_RECOVERY_DELETE_ATTEMPTS_SIZE];
} object_recovery_deletion_record_t;

uint32_t object_recovery_boot_id(void);
bool object_recovery_get_last_deletion_record(
    object_recovery_deletion_record_t *record_out
);


/*
 * Inspect persisted object directories and recover interrupted metadata writes
 * where possible. Directories that match OBJ_XXXX but contain no valid object
 * metadata are removed as incomplete objects.
 *
 * This synchronous entry point is retained for tests/tools. Production boot
 * uses object_recovery_run_active() followed by object_recovery_start_background()
 * so a large SD archive cannot hold the interactive startup path for seconds.
 */
esp_err_t object_recovery_run(void);


/*
 * Recover only the object referenced by active_object.txt. This is the only
 * recovery work allowed on the interactive boot path so object_store_init()
 * never restores an active object from corrupt/interrupted metadata.
 */
esp_err_t object_recovery_run_active(void);


/*
 * Start/stop the low-priority full-storage recovery worker. The worker scans
 * the object directory once, then validates each object inside its own bounded
 * storage_manager operation so TFT/LVGL can resume between objects.
 */
esp_err_t object_recovery_start_background(void);

/*
 * Variant used by startup orchestration. The callback runs in the recovery
 * worker after the archive pass finishes (successfully or with an error), but
 * is skipped when shutdown explicitly requests the worker to stop. The task
 * remains alive until the callback returns, so shutdown cannot tear services
 * down underneath the callback.
 */
esp_err_t object_recovery_start_background_with_callback(
    object_recovery_background_complete_fn complete_fn,
    void *user_context
);

void object_recovery_stop_background(void);
bool object_recovery_is_running(void);
