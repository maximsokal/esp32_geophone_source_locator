#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_format.h"

#define OBJECT_STORE_POD_COUNT 4

typedef struct
{
    bool computed;
    bool leveled;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    int32_t residual_mm;
    int32_t uncertainty_mm;
    uint64_t computed_at_ms;

} object_store_pod_geometry_t;

typedef struct
{
    bool anchored;
    object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT];

} object_store_geometry_t;

#define OBJECT_STORE_MAX_CS_PAIRS 10U

typedef struct
{
    uint8_t node_a;
    uint8_t node_b;
    bool valid;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t attempts;
    uint8_t valid_samples;
    int32_t spread_mm;
    uint8_t rejected_samples;

} object_store_cs_pair_t;

typedef struct
{
    size_t pair_count;
    object_store_cs_pair_t pairs[OBJECT_STORE_MAX_CS_PAIRS];

} object_store_cs_pairs_t;

typedef struct
{
    uint32_t sessions;
    uint32_t artifacts;
    uint32_t localization_runs;
    uint64_t bytes;

} object_store_delete_scope_t;

typedef struct
{
    object_summary_t summary;
    object_store_cs_pairs_t cs_pairs;

} object_store_profile_t;

extern bool object_recovery_test_object_leased;

esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
);

esp_err_t object_store_get_delete_scope(
    const char *object_id,
    object_store_delete_scope_t *scope
);

static inline esp_err_t object_store_object_is_leased(
    const char *object_id,
    bool *leased
)
{
    (void)object_id;
    if (leased == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *leased = object_recovery_test_object_leased;
    return ESP_OK;
}
