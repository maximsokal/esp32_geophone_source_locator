#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * run_chunk holds one mutex for the whole callback. enter_protected takes
 * that same mutex before setting the latch, so it returns only after any
 * earlier chunk finishes; later chunks observe the latch and do not run.
 * The callback runs while the mutex is held and must not call this API.
 */
esp_err_t storage_bulk_admission_enter_protected(void);

void storage_bulk_admission_exit_protected(void);

bool storage_bulk_admission_is_protected(void);

esp_err_t storage_bulk_admission_run_chunk(
    esp_err_t (*fn)(void *),
    void *ctx
);
