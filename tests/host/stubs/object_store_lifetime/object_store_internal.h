#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t object_store_internal_begin_file_operation(void);
void object_store_internal_end_file_operation(void);
esp_err_t object_store_internal_begin_state_operation(void);
void object_store_internal_end_state_operation(void);
bool object_store_fs_object_exists(const char *object_id);
