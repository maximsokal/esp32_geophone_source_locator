#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_format.h"

#define OBJECT_STORE_ID_SIZE OBJECT_FORMAT_ID_SIZE
#define OBJECT_STORE_NAME_SIZE OBJECT_FORMAT_NAME_SIZE
#define OBJECT_STORE_MAX_OBJECTS OBJECT_FORMAT_MAX_OBJECTS
#define OBJECT_STORE_POD_COUNT 4
#define OBJECT_STORE_LOCATION_SOURCE_SIZE 24
#define OBJECT_STORE_DEFAULT_CABLE_LENGTH_M 5.0f
#define OBJECT_STORE_MAX_LEASES 4U

typedef object_summary_t object_store_summary_t;

typedef struct
{
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

/*
 * Per-pod relative geometry (MAX-118): X/Y from the Channel Sounding
 * distance mesh, Z from the BME280 altitude subtraction (MAX-114). MAIN is
 * always the implicit origin and is not itself a pod. `computed` is false
 * for a pod that has no path of valid CS pairs back to MAIN -- do not
 * treat x_mm/y_mm/z_mm as meaningful unless computed is true. `leveled`
 * mirrors that pod's nRF orientation_state == ACCEPTED at the moment the
 * geometry was computed, not a live value.
 */
typedef struct
{
    bool computed;
    bool leveled;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    uint64_t computed_at_ms;
} object_store_pod_geometry_t;

/*
 * `anchored` is always false until a later GPS/AprilTag step (not yet
 * implemented) provides an absolute reference -- distance-only Channel
 * Sounding cannot fix compass heading, so the layout has the right
 * relative shape but an arbitrary rotation/mirror until then.
 */
typedef struct
{
    bool anchored;
    object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT];
} object_store_geometry_t;

/*
 * MAX-148: the raw Channel Sounding pair evidence that produced `geometry`
 * above, for a WebUI placement-confidence view -- distinct from the solved
 * X/Y/Z, which is all `object_store_pod_geometry_t` keeps. node_a/node_b are
 * the physical node indices (0 = MAIN, 1..4 = REMOTE pods 1..4) this pair
 * measured between. `valid` mirrors the CS scheduler's own VALID state (not
 * PENDING/MISSING/RETRY_EXHAUSTED); a pair the scheduler never resolved is
 * simply absent from the array rather than present with fabricated values.
 * confidence is 0..1000 (the CS provider's own scale, passed through
 * unmodified -- never rescaled/reinterpreted here).
 */
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
} object_store_cs_pair_t;

typedef struct
{
    size_t pair_count;
    object_store_cs_pair_t pairs[OBJECT_STORE_MAX_CS_PAIRS];
} object_store_cs_pairs_t;

typedef struct
{
    object_store_summary_t summary;
    object_store_location_t location;
    float cable_lengths_m[OBJECT_STORE_POD_COUNT];
    object_store_geometry_t geometry;
    object_store_cs_pairs_t cs_pairs;
} object_store_profile_t;

/*
 * Opaque process-lifetime anti-delete ownership token. A zero token means the
 * caller holds no lease. Multiple independent owners may lease the same object
 * concurrently; leases never serialize ordinary I/O or non-destructive object
 * mutations.
 */
typedef struct
{
    uint64_t token_id;
} object_store_lease_t;

/*
 * Global application-data destructive reservation. While owned, no new object
 * lifetime lease can be acquired. Acquisition succeeds only when no lease is
 * active, closing the ERASE-vs-recorder/scan race in both directions without
 * holding storage/TFT arbitration for the erase lifetime.
 */
typedef struct
{
    uint64_t token_id;
} object_store_destructive_reservation_t;

esp_err_t object_store_init(void);
esp_err_t object_store_create(object_store_summary_t *created_object);
esp_err_t object_store_create_named(const char *initial_name, object_store_summary_t *created_object);
esp_err_t object_store_list(object_store_summary_t *objects, size_t max_objects, size_t *object_count);
esp_err_t object_store_list_profiles(object_store_profile_t *objects, size_t max_objects, size_t *object_count);
esp_err_t object_store_list_profiles_with_defaults(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
);

/*
 * Cooperative status/UI snapshot. Unlike the authoritative cold index build,
 * this releases the shared TFT/SD storage guard between object metadata files
 * so low-priority Web cache work cannot freeze the local LVGL UI for seconds.
 */
esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
);

esp_err_t object_store_get_active(object_store_summary_t *object);
esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile);
esp_err_t object_store_set_active(const char *object_id);
esp_err_t object_store_delete(const char *object_id);
esp_err_t object_store_rename(const char *object_id, const char *name);
esp_err_t object_store_set_location(const char *object_id, const object_store_location_t *location);
esp_err_t object_store_set_cable_length(const char *object_id, size_t pod_index, float length_m);
esp_err_t object_store_get_cable_defaults(float lengths_m[OBJECT_STORE_POD_COUNT]);
esp_err_t object_store_set_cable_default(size_t pod_index, float length_m);

/*
 * Replace this object's stored geometry wholesale (written once when the
 * recording-gate solve completes, not incrementally). Passing NULL clears
 * it back to all-pods-not-computed.
 */
esp_err_t object_store_set_geometry(const char *object_id, const object_store_geometry_t *geometry);

/*
 * Replace this object's stored CS pair evidence wholesale (written once
 * alongside object_store_set_geometry(), from the same completed CS cycle
 * that produced the geometry solve). Passing NULL clears it back to empty.
 */
esp_err_t object_store_set_cs_pairs(const char *object_id, const object_store_cs_pairs_t *cs_pairs);

/*
 * Shared anti-delete lease. Acquisition validates committed object existence
 * atomically with respect to object DELETE. Release is RAM-only and does not
 * touch FATFS/storage arbitration. The same object may have several leases.
 */
esp_err_t object_store_lease_acquire(
    const char *object_id,
    object_store_lease_t *lease
);
esp_err_t object_store_lease_release(
    object_store_lease_t *lease
);
esp_err_t object_store_object_is_leased(
    const char *object_id,
    bool *leased
);
esp_err_t object_store_any_leases_active(
    bool *active
);

esp_err_t object_store_destructive_reservation_acquire(
    object_store_destructive_reservation_t *reservation
);
esp_err_t object_store_destructive_reservation_release(
    object_store_destructive_reservation_t *reservation
);

/* RAM-only invalidation after out-of-band recovery changes. This does not
 * touch persisted/runtime ACTIVE authority and performs no filesystem work. */
esp_err_t object_store_invalidate_profile_cache(void);

esp_err_t object_store_reset(void);
