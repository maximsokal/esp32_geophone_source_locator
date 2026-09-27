#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "geophone_events.h"
#include "object_store.h"

/* Tune these defaults with real recordings; TS mirrors the same values. */
#define RESULT_VIEW_MINIMUM_WINDOW_S 2.0
#define RESULT_VIEW_DEFAULT_PRE_S 0.5
#define RESULT_VIEW_DEFAULT_POST_S 1.0

typedef enum {
    RESULT_VIEW_OK = 0,
    RESULT_VIEW_UNAVAILABLE,
    RESULT_VIEW_INVALID_ARGUMENT,
} result_view_status_t;

typedef struct {
    bool available;
    size_t start_frame;
    size_t frame_count;
} result_view_window_t;

typedef struct {
    bool known;
    /* NaN when !known; callers must check known before displaying this. */
    double distance_m;
} result_view_distance_t;

/* `picks` may be NULL only when pick_count is zero. End frame is exclusive. */
result_view_status_t result_view_segment_window(
    const geophone_channel_pick_t *picks,
    size_t pick_count,
    uint32_t sample_rate_hz,
    size_t session_frame_count,
    double pre_s,
    double post_s,
    result_view_window_t *window
);

/* Returns true only when this accepted pick falls inside the available window. */
bool result_view_arrival_marker_offset(
    const geophone_channel_pick_t *pick,
    const result_view_window_t *window,
    size_t *offset_frames
);

/* Writes one entry per pod; non-computed pod distances are unknown (NaN). */
result_view_status_t result_view_source_distances(
    const double source_m[3],
    const object_store_pod_geometry_t *pods,
    size_t pod_count,
    result_view_distance_t *distances
);
