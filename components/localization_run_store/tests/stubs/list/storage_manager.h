#pragma once

#include <stddef.h>

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "localization_run_store_list_tmp"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);
esp_err_t storage_manager_atomic_write_text(const char *path, const char *text);
