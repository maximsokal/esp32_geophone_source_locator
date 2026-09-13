#pragma once

#include <stdbool.h>

#include "esp_err.h"


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
void object_recovery_stop_background(void);
bool object_recovery_is_running(void);
