#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OBJECT_STORE_ID_SIZE 16U
#define OBJECT_STORE_POD_COUNT 4U
#define OBJECT_STORE_LOCATION_SOURCE_SIZE 24U

typedef struct {
    bool active;
    char id[OBJECT_STORE_ID_SIZE];
} object_store_summary_t;

typedef struct {
    bool valid;
    double latitude;
    double longitude;
    float accuracy_m;
    bool altitude_valid;
    double altitude_m;
    float altitude_accuracy_m;
    uint64_t captured_at_ms;
    char source[OBJECT_STORE_LOCATION_SOURCE_SIZE];
} object_store_location_t;

typedef struct {
    int unused;
} object_store_profile_t;

typedef struct {
    uint64_t token_id;
} object_store_lease_t;

typedef struct {
    bool computed;
    bool leveled;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    int32_t residual_mm;
    uint64_t computed_at_ms;
} object_store_pod_geometry_t;

typedef struct {
    bool anchored;
    object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT];
} object_store_geometry_t;

#define OBJECT_STORE_MAX_CS_PAIRS 10U

typedef struct {
    uint8_t node_a;
    uint8_t node_b;
    bool valid;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t attempts;
    uint8_t valid_samples;
} object_store_cs_pair_t;

typedef struct {
    size_t pair_count;
    object_store_cs_pair_t pairs[OBJECT_STORE_MAX_CS_PAIRS];
} object_store_cs_pairs_t;

esp_err_t object_store_create_named(const char *name, object_store_summary_t *created_object);
esp_err_t object_store_set_active(const char *object_id);
esp_err_t object_store_delete(const char *object_id);
esp_err_t object_store_rename(const char *object_id, const char *name);
esp_err_t object_store_set_location(const char *object_id, const object_store_location_t *location);
esp_err_t object_store_set_cable_length(const char *object_id, size_t pod_index, float length_m);
esp_err_t object_store_set_cable_default(size_t pod_index, float length_m);
esp_err_t object_store_lease_acquire(const char *object_id, object_store_lease_t *lease);
esp_err_t object_store_lease_release(object_store_lease_t *lease);
