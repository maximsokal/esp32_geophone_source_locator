#pragma once

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "test-storage"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

/*
 * localization_calculate_run()'s only two storage_manager entry points
 * (load_session_occupancy() arbitrates around a raw fopen/fread of the
 * scan occupancy artifact -- it never goes through
 * storage_manager_atomic_write_text/read_text, so this fake does not need
 * to provide those). The test .c file defines these as trivial no-ops.
 */
esp_err_t storage_manager_operation_begin(void);
void storage_manager_operation_end(void);
