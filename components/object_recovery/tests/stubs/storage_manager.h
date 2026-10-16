#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifndef STORAGE_MANAGER_MOUNT_POINT
#define STORAGE_MANAGER_MOUNT_POINT "object_recovery_contract_tmp"
#endif
#ifndef STORAGE_MANAGER_ROOT_PATH
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#endif
#ifndef STORAGE_MANAGER_OBJECTS_PATH
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"
#endif

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);
esp_err_t storage_manager_remove_tree(const char *path);
