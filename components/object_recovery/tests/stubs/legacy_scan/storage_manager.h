#pragma once

#include "esp_err.h"

#ifndef STORAGE_MANAGER_MOUNT_POINT
#define STORAGE_MANAGER_MOUNT_POINT \
    "object_recovery_legacy_scan_tmp"
#endif

#ifndef STORAGE_MANAGER_ROOT_PATH
#define STORAGE_MANAGER_ROOT_PATH \
    STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#endif

#ifndef STORAGE_MANAGER_OBJECTS_PATH
#define STORAGE_MANAGER_OBJECTS_PATH \
    STORAGE_MANAGER_ROOT_PATH "/objects"
#endif

esp_err_t storage_manager_operation_begin(void);
void storage_manager_operation_end(void);
