#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OBJECT_STORE_ID_SIZE 9U
#define OBJECT_STORE_MAX_LEASES 4U

typedef struct { uint64_t token_id; } object_store_lease_t;
typedef struct { uint64_t token_id; } object_store_destructive_reservation_t;

esp_err_t object_store_lease_acquire(const char *object_id, object_store_lease_t *lease);
esp_err_t object_store_lease_release(object_store_lease_t *lease);
esp_err_t object_store_object_is_leased(const char *object_id, bool *leased);
esp_err_t object_store_any_leases_active(bool *active);
esp_err_t object_store_destructive_reservation_acquire(object_store_destructive_reservation_t *reservation);
esp_err_t object_store_destructive_reservation_release(object_store_destructive_reservation_t *reservation);
