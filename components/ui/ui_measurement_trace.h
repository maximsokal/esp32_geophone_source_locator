#pragma once

#include <stddef.h>
#include <stdint.h>

#include "seismic_buffer.h"


#define UI_MEASUREMENT_TRACE_POINTS  48U


/*
 * Compress a recent seismic frame window into display points without losing
 * short impulses. Each time bucket keeps the signed sample with the greatest
 * absolute magnitude for every channel.
 *
 * When fewer frames than display points are available, samples are right
 * aligned so the newest data remains at the right edge of the chart.
 * Returns the greatest absolute input magnitude in microvolts.
 */
float ui_measurement_trace_reduce(
    const seismic_frame_t *frames,
    size_t frame_count,
    int32_t trace[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ]
);
