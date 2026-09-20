#include "localization_event_bridge.h"

#include <math.h>


#define LOCALIZATION_EVENT_MAX_CHANNELS 32u


esp_err_t localization_event_solve(
    const localization_event_channel_input_t *channels,
    size_t channel_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_event_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = (localization_event_result_t) { 0 };

    if (
        channels == NULL ||
        config == NULL ||
        channel_count == 0u ||
        channel_count > LOCALIZATION_EVENT_MAX_CHANNELS
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_observation_t observations[LOCALIZATION_EVENT_MAX_CHANNELS];
    size_t used_count = 0u;

    for (size_t i = 0u; i < channel_count; ++i) {
        if (channels[i].pick.status != GEOPHONE_PICK_ACCEPTED) {
            result->channel_mask_rejected |= (1u << i);
            continue;
        }

        if (channels[i].sample_rate_hz == 0u) {
            /* An accepted pick with no known sample rate cannot be
             * converted to an absolute time; treat it like any other
             * unusable channel rather than guessing a rate. */
            result->channel_mask_rejected |= (1u << i);
            continue;
        }

        result->channel_mask_used |= (1u << i);

        /*
         * Convert via the exact sample index and sample rate rather than
         * the pick's own float32 arrival_time_s: at tens of seconds into a
         * recording, float32 no longer resolves single microseconds, which
         * would quietly degrade TDOA precision.
         */
        const double arrival_time_us =
            (double)channels[i].pick.arrival_sample_index *
            1.0e6 /
            (double)channels[i].sample_rate_hz;

        observations[used_count] = (localization_observation_t) {
            .sensor_position = channels[i].sensor_position,
            .arrival_time_us = (int64_t)llround(arrival_time_us)
        };
        ++used_count;
    }

    result->channels_used_count = used_count;

    if (used_count < LOCALIZATION_MIN_SENSORS_3D) {
        result->status = LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS;
        return ESP_OK;
    }

    const esp_err_t solve_err = localization_tdoa_solve(
        observations,
        used_count,
        config,
        initial_guess,
        &result->solver_result
    );

    if (solve_err != ESP_OK) {
        result->status = LOCALIZATION_EVENT_SOLVER_REJECTED;
        return ESP_OK;
    }

    result->status = LOCALIZATION_EVENT_SOLVED;
    return ESP_OK;
}
