#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "object_store_profile_contract_tmp"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

bool storage_manager_path_exists(const char *path);
esp_err_t storage_manager_path_status(const char *path, bool *exists);
esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);
esp_err_t storage_manager_atomic_write_text(const char *path, const char *text);
esp_err_t storage_manager_operation_begin(void);
void storage_manager_operation_end(void);
