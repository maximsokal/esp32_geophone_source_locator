#pragma once

#include <stddef.h>

#include "esp_err.h"

#define STORAGE_MANAGER_OBJECTS_PATH "/wfS/objects"

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text);
esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);
