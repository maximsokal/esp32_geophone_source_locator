#include "object_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "object_format.h"
#include "object_store_internal.h"


typedef struct
{
    uint64_t token_id;
    char object_id[OBJECT_STORE_ID_SIZE];

} object_store_lease_slot_t;


static const char *LEASE_TAG = "OBJECT_STORE_LEASE";

static object_store_lease_slot_t s_leases[OBJECT_STORE_MAX_LEASES];
static uint64_t s_destructive_token_id = 0U;
static uint64_t s_next_token_id = 1U;


static bool object_store_any_lease_held_locked(void)
{
    for (size_t index = 0U; index < OBJECT_STORE_MAX_LEASES; ++index) {
        if (s_leases[index].token_id != 0U) {
            return true;
        }
    }
    return false;
}


static bool object_store_lease_token_in_use_locked(uint64_t token_id)
{
    if (token_id == 0U) {
        return false;
    }
    if (s_destructive_token_id == token_id) {
        return true;
    }

    for (size_t index = 0U; index < OBJECT_STORE_MAX_LEASES; ++index) {
        if (s_leases[index].token_id == token_id) {
            return true;
        }
    }
    return false;
}


static uint64_t object_store_lease_next_token_locked(void)
{
    for (;;) {
        uint64_t token_id = s_next_token_id++;
        if (s_next_token_id == 0U) {
            s_next_token_id = 1U;
        }
        if (token_id == 0U) {
            continue;
        }
        if (!object_store_lease_token_in_use_locked(token_id)) {
            return token_id;
        }
    }
}


void object_store_lease_reset_locked(void)
{
    memset(s_leases, 0, sizeof(s_leases));
    s_destructive_token_id = 0U;
    s_next_token_id = 1U;
}


bool object_store_lease_is_object_held_locked(const char *object_id)
{
    if (!object_format_id_is_valid(object_id)) {
        return false;
    }

    for (size_t index = 0U; index < OBJECT_STORE_MAX_LEASES; ++index) {
        if (
            s_leases[index].token_id != 0U &&
            strcmp(s_leases[index].object_id, object_id) == 0
        ) {
            return true;
        }
    }
    return false;
}


esp_err_t object_store_lease_acquire(
    const char *object_id,
    object_store_lease_t *lease
)
{
    if (!object_format_id_is_valid(object_id) || lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->token_id != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) {
        return err;
    }

    if (s_destructive_token_id != 0U) {
        object_store_internal_end_file_operation();
        return ESP_ERR_INVALID_STATE;
    }

    if (!object_store_fs_object_exists(object_id)) {
        object_store_internal_end_file_operation();
        return ESP_ERR_NOT_FOUND;
    }

    size_t free_index = OBJECT_STORE_MAX_LEASES;
    for (size_t index = 0U; index < OBJECT_STORE_MAX_LEASES; ++index) {
        if (s_leases[index].token_id == 0U) {
            free_index = index;
            break;
        }
    }

    if (free_index == OBJECT_STORE_MAX_LEASES) {
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }

    const uint64_t token_id = object_store_lease_next_token_locked();
    s_leases[free_index].token_id = token_id;
    (void)snprintf(
        s_leases[free_index].object_id,
        sizeof(s_leases[free_index].object_id),
        "%s",
        object_id
    );
    lease->token_id = token_id;

    object_store_internal_end_file_operation();

    ESP_LOGI(
        LEASE_TAG,
        "Lease acquired object=%s token=%llu",
        object_id,
        (unsigned long long)token_id
    );
    return ESP_OK;
}


esp_err_t object_store_lease_release(object_store_lease_t *lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->token_id == 0U) {
        return ESP_OK;
    }

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t token_id = lease->token_id;
    size_t found_index = OBJECT_STORE_MAX_LEASES;
    for (size_t index = 0U; index < OBJECT_STORE_MAX_LEASES; ++index) {
        if (s_leases[index].token_id == token_id) {
            found_index = index;
            break;
        }
    }

    if (found_index == OBJECT_STORE_MAX_LEASES) {
        object_store_internal_end_state_operation();
        return ESP_ERR_INVALID_STATE;
    }

    char object_id[OBJECT_STORE_ID_SIZE];
    (void)snprintf(
        object_id,
        sizeof(object_id),
        "%s",
        s_leases[found_index].object_id
    );
    memset(&s_leases[found_index], 0, sizeof(s_leases[found_index]));
    lease->token_id = 0U;

    object_store_internal_end_state_operation();

    ESP_LOGI(
        LEASE_TAG,
        "Lease released object=%s token=%llu",
        object_id,
        (unsigned long long)token_id
    );
    (void)object_id;
    return ESP_OK;
}


esp_err_t object_store_object_is_leased(
    const char *object_id,
    bool *leased
)
{
    if (!object_format_id_is_valid(object_id) || leased == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) {
        return err;
    }

    *leased = object_store_lease_is_object_held_locked(object_id);

    object_store_internal_end_state_operation();
    return ESP_OK;
}


esp_err_t object_store_any_leases_active(bool *active)
{
    if (active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) {
        return err;
    }

    *active = object_store_any_lease_held_locked();

    object_store_internal_end_state_operation();
    return ESP_OK;
}


esp_err_t object_store_destructive_reservation_acquire(
    object_store_destructive_reservation_t *reservation
)
{
    if (reservation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reservation->token_id != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) {
        return err;
    }

    if (s_destructive_token_id != 0U || object_store_any_lease_held_locked()) {
        object_store_internal_end_state_operation();
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t token_id = object_store_lease_next_token_locked();
    s_destructive_token_id = token_id;
    reservation->token_id = token_id;

    object_store_internal_end_state_operation();

    ESP_LOGI(
        LEASE_TAG,
        "Destructive reservation acquired token=%llu",
        (unsigned long long)token_id
    );
    return ESP_OK;
}


esp_err_t object_store_destructive_reservation_release(
    object_store_destructive_reservation_t *reservation
)
{
    if (reservation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reservation->token_id == 0U) {
        return ESP_OK;
    }

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t token_id = reservation->token_id;
    if (s_destructive_token_id != token_id) {
        object_store_internal_end_state_operation();
        return ESP_ERR_INVALID_STATE;
    }

    s_destructive_token_id = 0U;
    reservation->token_id = 0U;

    object_store_internal_end_state_operation();

    ESP_LOGI(
        LEASE_TAG,
        "Destructive reservation released token=%llu",
        (unsigned long long)token_id
    );
    return ESP_OK;
}
