#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sd_card_info.h"
#include "seismic_buffer.h"
#include "seismic_recorder_storage_preflight.h"
#include "storage_manager.h"


static esp_err_t s_begin_result = ESP_OK;
static esp_err_t s_info_result = ESP_OK;
static uint64_t s_free_bytes = 0U;
static unsigned s_begin_calls = 0U;
static unsigned s_end_calls = 0U;
static unsigned s_info_calls = 0U;


static void reset_fake_storage(void)
{
    s_begin_result = ESP_OK;
    s_info_result = ESP_OK;
    s_free_bytes = 0U;
    s_begin_calls = 0U;
    s_end_calls = 0U;
    s_info_calls = 0U;
}


esp_err_t storage_manager_operation_begin(void)
{
    ++s_begin_calls;
    return s_begin_result;
}


void storage_manager_operation_end(void)
{
    ++s_end_calls;
}


esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    ++s_info_calls;

    if (s_info_result != ESP_OK) {
        return s_info_result;
    }

    assert(info != NULL);
    memset(info, 0, sizeof(*info));
    info->total_bytes = UINT64_MAX;
    info->free_bytes = s_free_bytes;
    return ESP_OK;
}


static void test_required_bytes_policy(void)
{
    const uint64_t one_mib = 1024ULL * 1024ULL;

    assert(
        seismic_recorder_storage_required_bytes(
            500U,
            sizeof(seismic_frame_t),
            one_mib
        ) == 1768576ULL
    );

    assert(
        seismic_recorder_storage_required_bytes(
            5000U,
            sizeof(seismic_frame_t),
            one_mib
        ) == 8248576ULL
    );

    assert(
        seismic_recorder_storage_required_bytes(
            0U,
            sizeof(seismic_frame_t),
            one_mib
        ) == UINT64_MAX
    );

    assert(
        seismic_recorder_storage_required_bytes(
            UINT32_MAX,
            SIZE_MAX,
            UINT64_MAX
        ) == UINT64_MAX
    );
}


static void test_preflight_accepts_exact_boundary(void)
{
    reset_fake_storage();
    s_free_bytes = 1768576ULL;

    assert(seismic_recorder_storage_preflight(500U) == ESP_OK);
    assert(s_begin_calls == 1U);
    assert(s_info_calls == 1U);
    assert(s_end_calls == 1U);
}


static void test_preflight_rejects_insufficient_space(void)
{
    reset_fake_storage();
    s_free_bytes = 1768575ULL;

    assert(seismic_recorder_storage_preflight(500U) == ESP_ERR_NO_MEM);
    assert(s_begin_calls == 1U);
    assert(s_info_calls == 1U);
    assert(s_end_calls == 1U);
}


static void test_preflight_propagates_space_query_failure(void)
{
    reset_fake_storage();
    s_info_result = ESP_FAIL;

    assert(seismic_recorder_storage_preflight(500U) == ESP_FAIL);
    assert(s_begin_calls == 1U);
    assert(s_info_calls == 1U);
    assert(s_end_calls == 1U);
}


static void test_preflight_does_not_query_when_storage_guard_fails(void)
{
    reset_fake_storage();
    s_begin_result = ESP_ERR_INVALID_STATE;

    assert(
        seismic_recorder_storage_preflight(500U) ==
        ESP_ERR_INVALID_STATE
    );
    assert(s_begin_calls == 1U);
    assert(s_info_calls == 0U);
    assert(s_end_calls == 0U);
}


static void test_preflight_rejects_zero_sample_rate_without_storage_io(void)
{
    reset_fake_storage();

    assert(
        seismic_recorder_storage_preflight(0U) ==
        ESP_ERR_INVALID_ARG
    );
    assert(s_begin_calls == 0U);
    assert(s_info_calls == 0U);
    assert(s_end_calls == 0U);
}


int main(void)
{
    assert(sizeof(seismic_frame_t) == 24U);
    test_required_bytes_policy();
    test_preflight_accepts_exact_boundary();
    test_preflight_rejects_insufficient_space();
    test_preflight_propagates_space_query_failure();
    test_preflight_does_not_query_when_storage_guard_fails();
    test_preflight_rejects_zero_sample_rate_without_storage_io();
    return 0;
}
