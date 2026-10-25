#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* The object-store FS contract supplies an injectable clear-active stub in the
 * same host translation unit. Suppress the production definition there while
 * leaving firmware and ordinary component builds unchanged. */
#define OBJECT_STORE_FS_CLEAR_ACTIVE_EXTERNAL_STUB 1

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
esp_err_t storage_manager_path_status(const char *path, bool *exists);
bool storage_manager_is_ready(void);
void storage_manager_operation_end(void);
esp_err_t storage_manager_create_directory(const char *path);
esp_err_t storage_manager_create_directory_exclusive(const char *path);
esp_err_t storage_manager_remove_tree(const char *path);
