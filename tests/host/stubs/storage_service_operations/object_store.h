#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct
{
    uint64_t token_id;
} object_store_destructive_reservation_t;

esp_err_t object_store_destructive_reservation_acquire(
    object_store_destructive_reservation_t *reservation
);
esp_err_t object_store_destructive_reservation_release(
    object_store_destructive_reservation_t *reservation
);
esp_err_t object_store_reset(void);
