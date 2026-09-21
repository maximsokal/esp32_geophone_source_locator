#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "localization.h"

/*
 * MAX-138 (part 1/4): the "energy-only spatial/relative-strength baseline"
 * -- a classic amplitude-weighted centroid location. Unlike every other
 * method in this epic, it needs no arrival-time pick and no propagation
 * speed at all, only each channel's own representative signal energy over
 * the event window (e.g. peak or RMS amplitude, squared, over that
 * window -- the caller's own choice; this component only requires it be
 * >= 0). That makes it usable even when timing is too poor or too few
 * channels triggered for the other methods to run, which is exactly its
 * role as a "baseline" for comparison.
 *
 * position = sum(weight_i * energy_i * sensor_position_i) / sum(weight_i * energy_i)
 *
 * energy_concentration reports how much the single strongest channel
 * dominates the weighted total -- a source symmetrically bracketed by
 * several similarly-loud sensors gives an intrinsically ambiguous
 * centroid, while one sensor clearly dominating gives a more directional,
 * more trustworthy estimate. This is a derived diagnostic, not a
 * pass/fail threshold: no concentration value is treated as "too low" by
 * this component itself.
 */

#define LOCALIZATION_ENERGY_BASELINE_MAX_CHANNELS 8U


typedef struct
{
    localization_point3d_t sensor_position;

    /* Representative signal energy for this channel over the event window
     * (e.g. peak or RMS amplitude squared). Must be >= 0. */
    double energy;

    /* <= 0 excludes this channel entirely. */
    double weight;

} localization_energy_baseline_channel_t;


typedef struct
{
    bool found;
    localization_point3d_t position;
    double energy_concentration; /* in (0, 1]; see header comment */
    size_t channels_used_count;

} localization_energy_baseline_result_t;


/*
 * channel_count must be in [1, LOCALIZATION_ENERGY_BASELINE_MAX_CHANNELS].
 * found is false (not an error) if every channel is excluded or the total
 * weighted energy is exactly zero -- there is nothing to centroid.
 */
esp_err_t localization_energy_baseline_estimate(
    const localization_energy_baseline_channel_t *channels,
    size_t channel_count,
    localization_energy_baseline_result_t *result
);
