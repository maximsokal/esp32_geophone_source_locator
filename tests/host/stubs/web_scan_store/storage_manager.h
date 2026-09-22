#pragma once

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "web_scan_store_emulator_tmp"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

esp_err_t storage_manager_operation_begin(void);
void storage_manager_operation_end(void);
