#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "object_store_internal.h"


static esp_err_t s_settings_result = ESP_OK;
static unsigned s_reset_calls;
static unsigned s_default_notes;
static float s_restored_defaults[OBJECT_STORE_POD_COUNT];


esp_err_t object_store_settings_load_locked(
    float lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    if (s_settings_result != ESP_OK) {
        return s_settings_result;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        lengths_m[i] = (float)(i + 1U) * 1.25f;
    }
    return ESP_OK;
}


void object_store_profile_cache_reset(void)
{
    ++s_reset_calls;
}


void object_store_profile_cache_note_default_locked(
    size_t pod_index,
    float length_m
)
{
    assert(pod_index < OBJECT_STORE_POD_COUNT);
    s_restored_defaults[pod_index] = length_m;
    ++s_default_notes;
}


#include "../object_store_profile_cache_policy.c"


static void reset_trace(void)
{
    s_reset_calls = 0U;
    s_default_notes = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        s_restored_defaults[i] = 0.0f;
    }
}


int main(void)
{
    reset_trace();
    s_settings_result = ESP_OK;
    object_store_profile_cache_invalidate_membership_locked();

    assert(s_reset_calls == 1U);
    assert(s_default_notes == OBJECT_STORE_POD_COUNT);
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(s_restored_defaults[i] == (float)(i + 1U) * 1.25f);
    }

    reset_trace();
    s_settings_result = ESP_FAIL;
    object_store_profile_cache_invalidate_membership_locked();
    assert(s_reset_calls == 1U);
    assert(s_default_notes == 0U);

    puts("OBJECT_STORE_PROFILE_CACHE_POLICY_HOST_PASS");
    return 0;
}
