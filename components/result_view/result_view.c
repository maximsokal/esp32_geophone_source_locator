#include "result_view.h"

#include <math.h>
#include <stdint.h>

result_view_status_t result_view_segment_window(
    const geophone_channel_pick_t *picks,
    size_t pick_count,
    uint32_t sample_rate_hz,
    size_t session_frame_count,
    double pre_s,
    double post_s,
    result_view_window_t *window
)
{
    if (window == NULL) {
        return RESULT_VIEW_INVALID_ARGUMENT;
    }
    *window = (result_view_window_t){0};
    if ((pick_count > 0U && picks == NULL) || sample_rate_hz == 0U ||
        session_frame_count == 0U || !isfinite(pre_s) || pre_s < 0.0 ||
        !isfinite(post_s) || post_s < 0.0) {
        return RESULT_VIEW_INVALID_ARGUMENT;
    }

    const double pre_samples = pre_s * (double)sample_rate_hz;
    const double post_samples = post_s * (double)sample_rate_hz;
    const double minimum_samples = ceil(RESULT_VIEW_MINIMUM_WINDOW_S * (double)sample_rate_hz);
    if (!isfinite(pre_samples) || !isfinite(post_samples) ||
        pre_samples > (double)SIZE_MAX || post_samples > (double)SIZE_MAX ||
        !isfinite(minimum_samples) || minimum_samples > (double)SIZE_MAX) {
        return RESULT_VIEW_INVALID_ARGUMENT;
    }

    size_t first_arrival = 0U;
    size_t last_arrival = 0U;
    bool found = false;
    for (size_t i = 0U; i < pick_count; ++i) {
        if (picks[i].status != GEOPHONE_PICK_ACCEPTED) {
            continue;
        }
        const size_t arrival = picks[i].arrival_sample_index;
        if (arrival >= session_frame_count) {
            return RESULT_VIEW_INVALID_ARGUMENT;
        }
        if (!found || arrival < first_arrival) {
            first_arrival = arrival;
        }
        if (!found || arrival > last_arrival) {
            last_arrival = arrival;
        }
        found = true;
    }
    if (!found) {
        return RESULT_VIEW_UNAVAILABLE;
    }

    const double requested_start = (double)first_arrival - pre_samples;
    const double requested_end = (double)last_arrival + post_samples;
    const size_t start = requested_start <= 0.0 ? 0U : (size_t)floor(requested_start);
    size_t end = requested_end >= (double)session_frame_count
        ? session_frame_count
        : (size_t)ceil(requested_end);
    if (end <= last_arrival) {
        end = last_arrival + 1U;
    }

    const size_t minimum_frame_count = (size_t)minimum_samples;
    if (session_frame_count <= minimum_frame_count) {
        window->available = true;
        window->frame_count = session_frame_count;
        return RESULT_VIEW_OK;
    }

    size_t window_start = start;
    size_t window_frame_count = end - start;
    if (window_frame_count < minimum_frame_count) {
        const size_t left_expansion = (minimum_frame_count - window_frame_count) / 2U;
        window_start = start > left_expansion ? start - left_expansion : 0U;
        const size_t latest_start = session_frame_count - minimum_frame_count;
        if (window_start > latest_start) {
            window_start = latest_start;
        }
        window_frame_count = minimum_frame_count;
    }

    window->available = true;
    window->start_frame = window_start;
    window->frame_count = window_frame_count;
    return RESULT_VIEW_OK;
}

bool result_view_arrival_marker_offset(
    const geophone_channel_pick_t *pick,
    const result_view_window_t *window,
    size_t *offset_frames
)
{
    if (offset_frames != NULL) {
        *offset_frames = 0U;
    }
    if (pick == NULL || window == NULL || offset_frames == NULL || !window->available ||
        window->frame_count == 0U || pick->status != GEOPHONE_PICK_ACCEPTED ||
        pick->arrival_sample_index < window->start_frame) {
        return false;
    }
    const size_t offset = pick->arrival_sample_index - window->start_frame;
    if (offset >= window->frame_count) {
        return false;
    }
    *offset_frames = offset;
    return true;
}

result_view_status_t result_view_source_distances(
    const double source_m[3],
    const object_store_pod_geometry_t *pods,
    size_t pod_count,
    result_view_distance_t *distances
)
{
    if (pod_count > OBJECT_STORE_POD_COUNT) {
        return RESULT_VIEW_INVALID_ARGUMENT;
    }
    if (distances != NULL) {
        for (size_t i = 0U; i < pod_count; ++i) {
            distances[i] = (result_view_distance_t){.known = false, .distance_m = NAN};
        }
    }
    if (source_m == NULL || distances == NULL || (pod_count > 0U && pods == NULL) ||
        !isfinite(source_m[0]) || !isfinite(source_m[1]) || !isfinite(source_m[2])) {
        return RESULT_VIEW_INVALID_ARGUMENT;
    }

    for (size_t i = 0U; i < pod_count; ++i) {
        if (!pods[i].computed) {
            continue;
        }

        const double dx = source_m[0] - (double)pods[i].x_mm / 1000.0;
        const double dy = source_m[1] - (double)pods[i].y_mm / 1000.0;
        const double dz = source_m[2] - (double)pods[i].z_mm / 1000.0;
        distances[i].known = true;
        distances[i].distance_m = hypot(hypot(dx, dy), dz);
    }
    return RESULT_VIEW_OK;
}
