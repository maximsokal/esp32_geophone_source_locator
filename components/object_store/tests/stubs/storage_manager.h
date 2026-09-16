#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifndef STORAGE_MANAGER_MOUNT_POINT
#define STORAGE_MANAGER_MOUNT_POINT "object_store_contract_tmp"
#endif

#ifndef STORAGE_MANAGER_ROOT_PATH
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#endif

#ifndef STORAGE_MANAGER_OBJECTS_PATH
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"
#endif

bool storage_manager_path_exists(const char *path);
esp_err_t storage_manager_create_directory(const char *path);
esp_err_t storage_manager_create_directory_exclusive(const char *path);
esp_err_t storage_manager_remove_tree(const char *path);
