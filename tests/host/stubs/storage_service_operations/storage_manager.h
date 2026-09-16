#pragma once

#include "esp_err.h"

esp_err_t storage_manager_operation_begin(void);
void storage_manager_operation_end(void);
esp_err_t storage_manager_erase_application_data(void);
