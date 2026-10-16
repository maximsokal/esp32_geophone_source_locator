#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_service.h"
#include "seismic_recorder.h"


typedef enum
{
    MEASUREMENT_SERVICE_LIVE = 0,
    MEASUREMENT_SERVICE_STARTING,
    MEASUREMENT_SERVICE_RECORDING,
    MEASUREMENT_SERVICE_STOPPING,
    MEASUREMENT_SERVICE_REVIEW,
    MEASUREMENT_SERVICE_DELETING,
    MEASUREMENT_SERVICE_ERROR

} measurement_service_state_t;


typedef struct
{
    bool initialized;
    bool busy;
    bool active_object_present;

    measurement_service_state_t state;

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
    char object_name[OBJECT_FORMAT_NAME_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];

    seismic_recorder_stop_stage_t stop_stage;
    seismic_recorder_stop_stage_t stop_error_stage;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;

    esp_err_t last_error;
    esp_err_t stop_stage_error;

} measurement_service_snapshot_t;

typedef struct
{
    uint32_t peak_occupancy;
    uint32_t overflow_count;
} measurement_service_queue_stats_t;


typedef esp_err_t (*measurement_service_start_guard_callback_t)(
    void *user_context
);


typedef esp_err_t (*measurement_service_start_object_callback_t)(
    const char *object_id,
    void *user_context
);


typedef void (*measurement_service_start_reservation_release_callback_t)(
    void *user_context
);


typedef void (*measurement_service_storage_refresh_callback_t)(
    void *user_context
);


esp_err_t measurement_service_init(void);


esp_err_t measurement_service_shutdown(void);


esp_err_t measurement_service_set_start_guard(
    measurement_service_start_guard_callback_t callback,
    void *user_context
);


esp_err_t measurement_service_set_start_object_callback(
    measurement_service_start_object_callback_t callback,
    void *user_context
);


esp_err_t measurement_service_set_start_reservation_release_callback(
    measurement_service_start_reservation_release_callback_t callback,
    void *user_context
);


esp_err_t measurement_service_set_storage_refresh_callback(
    measurement_service_storage_refresh_callback_t callback,
    void *user_context
);


esp_err_t measurement_service_start_async(void);


esp_err_t measurement_service_start_object_async(
    const char *object_id
);


esp_err_t measurement_service_stop_async(void);


esp_err_t measurement_service_keep_async(void);


esp_err_t measurement_service_delete_async(void);


esp_err_t measurement_service_get_snapshot(
    measurement_service_snapshot_t *snapshot
);

esp_err_t measurement_service_get_queue_stats(
    measurement_service_queue_stats_t *stats
);

/*
 * Object-storage mutations that may alter the globally ACTIVE object are
 * allowed only while the measurement service is idle. REVIEW intentionally
 * remains mutable because object_id/object_name keep the reviewed session's
 * immutable owner rather than tracking later ACTIVE changes.
 */
bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot
);

/*
 * Acquisition shutdown must not take ownership of a recorder that belongs to
 * the measurement state machine. Include the submit-gate busy window because
 * an accepted START can still report LIVE until the worker enters STARTING.
 */
bool measurement_service_snapshot_blocks_acquisition_stop(
    const measurement_service_snapshot_t *snapshot
);


bool measurement_service_is_busy(void);
