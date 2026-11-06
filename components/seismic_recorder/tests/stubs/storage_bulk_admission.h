#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t storage_bulk_admission_enter_protected(void);
void storage_bulk_admission_exit_protected(void);
bool storage_bulk_admission_is_protected(void);
esp_err_t storage_bulk_admission_run_chunk(esp_err_t (*fn)(void *), void *ctx);
