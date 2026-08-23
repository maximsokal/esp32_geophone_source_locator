#pragma once

#include "device_status.h"


/*
 * Lock the shared device status model.
 *
 * The returned pointer remains valid only until
 * device_status_internal_unlock() is called.
 */
esp_err_t device_status_internal_lock(
    device_status_snapshot_t **status
);


/*
 * Unlock the shared device status model.
 */
void device_status_internal_unlock(void);