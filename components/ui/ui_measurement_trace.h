#pragma once

#include <stddef.h>
#include <stdint.h>

#include "seismic_buffer.h"


#define UI_MEASUREMENT_TRACE_POINTS  48U


/*
 * Compress a recent seismic frame window into a display-only min/max
 * envelope. The mean of each channel is removed from the local display copy
 * before bucketing; the input frames are never modified.
 *
 * When fewer frames than display points are available, samples are right
 * aligned so the newest data remains at the right edge of the chart.
 * Returns the greatest absolute display-envelope magnitude in microvolts.
 */
float ui_measurement_trace_reduce(
    const seismic_frame_t *frames,
    size_t frame_count,
    int32_t trace_min[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ],
    int32_t trace_max[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ]
);
