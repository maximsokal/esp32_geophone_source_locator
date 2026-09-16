#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "object_store.h"

static unsigned s_file_begin_calls;
static unsigned s_state_begin_calls;
static unsigned s_exists_calls;
static bool s_object_exists = true;

bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strlen(object_id) == 8U && strncmp(object_id, "OBJ_", 4U) == 0;
}

esp_err_t object_store_internal_begin_file_operation(void)
{
    ++s_file_begin_calls;
    return ESP_OK;
}

void object_store_internal_end_file_operation(void)
{
}

esp_err_t object_store_internal_begin_state_operation(void)
{
    ++s_state_begin_calls;
    return ESP_OK;
}

void object_store_internal_end_state_operation(void)
{
}

bool object_store_fs_object_exists(const char *object_id)
{
    (void)object_id;
    ++s_exists_calls;
    return s_object_exists;
}

static void test_active_leases_block_destructive_reservation(void)
{
    object_store_lease_t first = {0};
    object_store_lease_t second = {0};
    object_store_destructive_reservation_t reservation = {0};

    assert(object_store_lease_acquire("OBJ_0001", &first) == ESP_OK);
    assert(object_store_lease_acquire("OBJ_0001", &second) == ESP_OK);
    assert(first.token_id != 0U);
    assert(second.token_id != 0U);
    assert(first.token_id != second.token_id);

    assert(object_store_destructive_reservation_acquire(&reservation) == ESP_ERR_INVALID_STATE);
    assert(reservation.token_id == 0U);

    assert(object_store_lease_release(&first) == ESP_OK);
    assert(object_store_destructive_reservation_acquire(&reservation) == ESP_ERR_INVALID_STATE);
    assert(object_store_lease_release(&second) == ESP_OK);
}

static void test_reservation_blocks_new_lease_before_filesystem_probe(void)
{
    object_store_destructive_reservation_t reservation = {0};
    object_store_lease_t lease = {0};

    assert(object_store_destructive_reservation_acquire(&reservation) == ESP_OK);
    assert(reservation.token_id != 0U);

    s_exists_calls = 0U;
    assert(object_store_lease_acquire("OBJ_0001", &lease) == ESP_ERR_INVALID_STATE);
    assert(lease.token_id == 0U);
    assert(s_exists_calls == 0U);

    assert(object_store_destructive_reservation_release(&reservation) == ESP_OK);
    assert(reservation.token_id == 0U);

    assert(object_store_lease_acquire("OBJ_0001", &lease) == ESP_OK);
    assert(s_exists_calls == 1U);
    assert(object_store_lease_release(&lease) == ESP_OK);
}

static void test_exact_reservation_token_release(void)
{
    object_store_destructive_reservation_t first = {0};
    object_store_destructive_reservation_t stale = {0};
    object_store_destructive_reservation_t second = {0};
    object_store_lease_t lease = {0};

    assert(object_store_destructive_reservation_acquire(&first) == ESP_OK);
    stale = first;
    assert(object_store_destructive_reservation_release(&first) == ESP_OK);

    assert(object_store_destructive_reservation_acquire(&second) == ESP_OK);
    assert(second.token_id != 0U);
    assert(second.token_id != stale.token_id);

    assert(object_store_destructive_reservation_release(&stale) == ESP_ERR_INVALID_STATE);
    assert(object_store_lease_acquire("OBJ_0001", &lease) == ESP_ERR_INVALID_STATE);

    assert(object_store_destructive_reservation_release(&second) == ESP_OK);
    assert(object_store_destructive_reservation_release(&second) == ESP_OK);
    assert(object_store_lease_acquire("OBJ_0001", &lease) == ESP_OK);
    assert(object_store_lease_release(&lease) == ESP_OK);
}

int main(void)
{
    (void)s_file_begin_calls;
    (void)s_state_begin_calls;
    s_object_exists = true;

    test_active_leases_block_destructive_reservation();
    test_reservation_blocks_new_lease_before_filesystem_probe();
    test_exact_reservation_token_release();
    return 0;
}
