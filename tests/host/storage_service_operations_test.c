#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "storage_service_internal.h"
#include "object_store.h"
#include "power_control.h"
#include "sd_card_info.h"
#include "storage_manager.h"

static char s_trace[32];
static size_t s_trace_len;
static esp_err_t s_reserve_result;
static esp_err_t s_release_result;
static esp_err_t s_storage_begin_result;
static esp_err_t s_erase_result;
static esp_err_t s_reset_result;
static bool s_shutdown_requested;

static void trace(char value)
{
    assert(s_trace_len + 1U < sizeof(s_trace));
    s_trace[s_trace_len++] = value;
    s_trace[s_trace_len] = '\0';
}

static void reset_case(void)
{
    memset(s_trace, 0, sizeof(s_trace));
    s_trace_len = 0U;
    s_reserve_result = ESP_OK;
    s_release_result = ESP_OK;
    s_storage_begin_result = ESP_OK;
    s_erase_result = ESP_OK;
    s_reset_result = ESP_OK;
    s_shutdown_requested = false;
}

esp_err_t object_store_destructive_reservation_acquire(
    object_store_destructive_reservation_t *reservation
)
{
    trace('R');
    if (s_reserve_result == ESP_OK) reservation->token_id = 41U;
    return s_reserve_result;
}

esp_err_t object_store_destructive_reservation_release(
    object_store_destructive_reservation_t *reservation
)
{
    trace('L');
    if (s_release_result == ESP_OK) reservation->token_id = 0U;
    return s_release_result;
}

esp_err_t storage_manager_operation_begin(void)
{
    trace('B');
    return s_storage_begin_result;
}

void storage_manager_operation_end(void)
{
    trace('S');
}

esp_err_t storage_manager_erase_application_data(void)
{
    trace('E');
    return s_erase_result;
}

esp_err_t object_store_reset(void)
{
    trace('O');
    return s_reset_result;
}

bool power_control_is_shutdown_requested(void)
{
    trace('P');
    return s_shutdown_requested;
}

esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    (void)info;
    return ESP_OK;
}

static void test_busy_reservation_has_zero_destructive_side_effects(void)
{
    reset_case();
    s_reserve_result = ESP_ERR_INVALID_STATE;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_ERR_INVALID_STATE);
    assert(strcmp(s_trace, "RP") == 0);
}

static void test_storage_begin_failure_releases_reservation(void)
{
    reset_case();
    s_storage_begin_result = ESP_FAIL;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_FAIL);
    assert(strcmp(s_trace, "RBLP") == 0);
}

static void test_erase_failure_skips_reset_and_releases(void)
{
    reset_case();
    s_erase_result = ESP_FAIL;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_FAIL);
    assert(strcmp(s_trace, "RBESLP") == 0);
}

static void test_success_order(void)
{
    reset_case();

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_DONE);
    assert(event.result == ESP_OK);
    assert(strcmp(s_trace, "RBEOSLP") == 0);
}

static void test_reset_failure_unwinds_storage_and_reservation(void)
{
    reset_case();
    s_reset_result = ESP_FAIL;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_FAIL);
    assert(strcmp(s_trace, "RBEOSLP") == 0);
}

static void test_release_failure_is_reported_after_success(void)
{
    reset_case();
    s_release_result = ESP_ERR_INVALID_STATE;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_ERR_INVALID_STATE);
    assert(strcmp(s_trace, "RBEOSLP") == 0);
}

static void test_primary_failure_wins_over_release_cleanup_failure(void)
{
    reset_case();
    s_erase_result = ESP_FAIL;
    s_release_result = ESP_ERR_INVALID_STATE;

    storage_service_event_t event;
    assert(storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_FAILED);
    assert(event.result == ESP_FAIL);
    assert(strcmp(s_trace, "RBESLP") == 0);
}

static void test_shutdown_is_checked_after_release(void)
{
    reset_case();
    s_shutdown_requested = true;

    storage_service_event_t event;
    assert(!storage_service_process_erase(&event));
    assert(event.type == STORAGE_SERVICE_EVENT_ERASE_DONE);
    assert(strcmp(s_trace, "RBEOSLP") == 0);
}

int main(void)
{
    test_busy_reservation_has_zero_destructive_side_effects();
    test_storage_begin_failure_releases_reservation();
    test_erase_failure_skips_reset_and_releases();
    test_success_order();
    test_reset_failure_unwinds_storage_and_reservation();
    test_release_failure_is_reported_after_success();
    test_primary_failure_wins_over_release_cleanup_failure();
    test_shutdown_is_checked_after_release();
    return 0;
}
