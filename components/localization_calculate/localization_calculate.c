#include "localization_calculate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "localization_calculate_policy.h"
#include "localization_event_run_record.h"
#include "localization_run_store.h"
#include "seismic_dsp.h"
#include "seismic_recorder.h"


/*
 * Bounds how long a single finalized session this build can pull into
 * PSRAM for offline analysis in one call, matching
 * seismic_recorder_read_session_raw()'s own "fail closed rather than
 * silently truncate" contract. 30000 frames is 60 s at this project's own
 * measured ~500 Hz acquisition rate (MAX-141/142) -- generous headroom for
 * one impact/tap event capture, not a guess at a physical property.
 */
#define LOCALIZATION_CALCULATE_MAX_FRAMES 30000U

/* A numerical search-strategy knob (coarse-to-refine grid narrowing), not a
 * physical measurement -- see localization_calculate_search_bounds()'s own
 * doc comment for the same distinction applied to the search box itself.
 * Both grid-search backends require refine_divisor >= 2. */
#define LOCALIZATION_CALCULATE_REFINE_DIVISOR 4U

#define LOCALIZATION_CALCULATE_METHOD_VERSION 1U

#define LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK \
    ((1U << OBJECT_STORE_POD_COUNT) - 1U)


static void free_channel_buffers(
    float *channel_samples[OBJECT_STORE_POD_COUNT]
)
{
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (channel_samples[i] != NULL) {
            heap_caps_free(channel_samples[i]);
            channel_samples[i] = NULL;
        }
    }
}


esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        config == NULL ||
        summary_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary_out, 0, sizeof(*summary_out));

    if (
        !(config->propagation_speed_mps > 0.0) ||
        !(config->propagation_speed_uncertainty_mps >= 0.0) ||
        !(config->timing_uncertainty_us > 0.0) ||
        config->semblance_window_half_span_samples == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    esp_err_t err = seismic_recorder_read_session_geometry(object_id, session_id, &geometry);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t all_channel_mask = 0U;
    const size_t participating_count =
        localization_calculate_participating_channels(&geometry, &all_channel_mask);
    summary_out->participating_channel_count = participating_count;

    const localization_dimensionality_t dimensionality =
        localization_dimensionality_classify(participating_count);
    if (dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT) {
        return ESP_ERR_INVALID_STATE;
    }

    /* --- Read this session's full-resolution raw samples ---------------- */

    seismic_frame_t *frames = heap_caps_malloc(
        sizeof(*frames) * LOCALIZATION_CALCULATE_MAX_FRAMES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (frames == NULL) {
        return ESP_ERR_NO_MEM;
    }

    seismic_recorder_session_summary_t session_summary;
    size_t frame_count = 0U;
    err = seismic_recorder_read_session_raw(
        object_id, session_id, frames, LOCALIZATION_CALCULATE_MAX_FRAMES,
        &frame_count, &session_summary
    );
    if (err != ESP_OK) {
        heap_caps_free(frames);
        return err;
    }
    if (frame_count == 0U || session_summary.sample_rate_hz == 0U) {
        heap_caps_free(frames);
        return ESP_ERR_INVALID_STATE;
    }

    /* --- Per-channel DC removal, energy, and arrival picking ------------- */

    float *channel_samples[OBJECT_STORE_POD_COUNT] = { 0 };
    localization_event_channel_input_t event_channels[OBJECT_STORE_POD_COUNT];
    memset(event_channels, 0, sizeof(event_channels));

    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        event_channels[ch].sample_rate_hz = session_summary.sample_rate_hz;

        if (!geometry.pods[ch].computed) {
            /* No valid sensor position for this channel: it was never a
             * candidate to pick from at all, so it is excluded exactly like
             * any other channel with no usable trigger. */
            event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
            continue;
        }

        /* Defensive default: GEOPHONE_PICK_ACCEPTED is enum value 0, so an
         * untouched pick must never be left at its zeroed state below --
         * that would silently read back as an accepted, zero-arrival-time
         * pick instead of "not yet computed". */
        event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
        event_channels[ch].sensor_position =
            localization_calculate_sensor_position(&geometry.pods[ch]);

        float *samples = heap_caps_malloc(
            sizeof(float) * frame_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (samples == NULL) {
            free_channel_buffers(channel_samples);
            heap_caps_free(frames);
            return ESP_ERR_NO_MEM;
        }
        channel_samples[ch] = samples;

        for (size_t i = 0U; i < frame_count; ++i) {
            samples[i] = frames[i].channel_uv[ch];
        }

        const seismic_dsp_config_t dsp_config = {
            .sample_rate_hz = session_summary.sample_rate_hz,
            .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL,
        };
        /* Both dsp calls only fail for a malformed config or NULL result,
         * neither possible with the fixed, parameter-free stages requested
         * here; still checked and failed closed rather than assumed, since
         * a failed DC removal must never be treated as a validated pick
         * candidate below. */
        err = seismic_dsp_init(&dsp_config);
        seismic_dsp_result_t dsp_result;
        if (err == ESP_OK) {
            err = seismic_dsp_process_block(samples, frame_count, false, &dsp_result);
        }
        if (err != ESP_OK) {
            free_channel_buffers(channel_samples);
            heap_caps_free(frames);
            return err;
        }

        geophone_event_pick_channel_arrival(
            samples, frame_count, session_summary.sample_rate_hz,
            &config->pick_config, &event_channels[ch].pick
        );
    }

    /* The raw frame buffer has now been de-interleaved into per-channel
     * DC-removed windows; nothing below needs the interleaved copy. */
    heap_caps_free(frames);
    frames = NULL;

    /* --- MAX-209: cross-channel physical consistency + xcorr refinement --- */

    const uint32_t sample_rate_hz = session_summary.sample_rate_hz;
    double max_sensor_distance_m = 0.0;
    for (size_t a = 0U; a < OBJECT_STORE_POD_COUNT; ++a) {
        for (size_t b = a + 1U; geometry.pods[a].computed && b < OBJECT_STORE_POD_COUNT; ++b) {
            if (!geometry.pods[b].computed) {
                continue;
            }
            const double dx = event_channels[a].sensor_position.x_m - event_channels[b].sensor_position.x_m;
            const double dy = event_channels[a].sensor_position.y_m - event_channels[b].sensor_position.y_m;
            const double dz = event_channels[a].sensor_position.z_m - event_channels[b].sensor_position.z_m;
            const double distance = sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > max_sensor_distance_m) {
                max_sensor_distance_m = distance;
            }
        }
    }

    {
        double arrival_s[OBJECT_STORE_POD_COUNT] = { 0 };
        localization_point3d_t gate_positions[OBJECT_STORE_POD_COUNT];
        uint32_t accepted_mask = 0U;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            gate_positions[ch] = event_channels[ch].sensor_position;
            if (event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED) {
                arrival_s[ch] =
                    localization_calculate_pick_sample_position(&event_channels[ch].pick) /
                    (double)sample_rate_hz;
                accepted_mask |= (1U << ch);
            }
        }
        /* 3 sigma of pick noise plus one sample of quantization. */
        const double tolerance_s =
            3.0 * config->timing_uncertainty_us / 1.0e6 + 1.0 / (double)sample_rate_hz;
        const uint32_t consistent_mask = localization_calculate_consistent_pick_mask(
            arrival_s, gate_positions, OBJECT_STORE_POD_COUNT, accepted_mask,
            config->propagation_speed_mps, config->propagation_speed_uncertainty_mps,
            tolerance_s
        );
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if ((accepted_mask & (1U << ch)) != 0U && (consistent_mask & (1U << ch)) == 0U) {
                event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_INCONSISTENT;
            }
        }

        /* Refine every other accepted pick's delay behind the earliest one
         * by cross-correlation (sub-sample); kept only when the waveforms
         * are similar and agree with the independent picks. */
        size_t reference_channel = OBJECT_STORE_POD_COUNT;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (
                event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED &&
                (reference_channel == OBJECT_STORE_POD_COUNT || arrival_s[ch] < arrival_s[reference_channel])
            ) {
                reference_channel = ch;
            }
        }
        const double v_min =
            config->propagation_speed_uncertainty_mps > 0.0 &&
            config->propagation_speed_mps > config->propagation_speed_uncertainty_mps
            ? config->propagation_speed_mps - config->propagation_speed_uncertainty_mps
            : config->propagation_speed_mps;
        const size_t max_lag_samples =
            (size_t)ceil(max_sensor_distance_m / v_min * (double)sample_rate_hz) + 2U;

        for (size_t ch = 0U; reference_channel < OBJECT_STORE_POD_COUNT && ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (ch == reference_channel || event_channels[ch].pick.status != GEOPHONE_PICK_ACCEPTED) {
                continue;
            }
            const double reference_position =
                localization_calculate_pick_sample_position(&event_channels[reference_channel].pick);
            const double pick_delay =
                localization_calculate_pick_sample_position(&event_channels[ch].pick) - reference_position;
            double refined_delay = 0.0;
            if (
                localization_calculate_xcorr_relative_delay(
                    channel_samples[reference_channel], channel_samples[ch], frame_count,
                    event_channels[reference_channel].pick.arrival_sample_index,
                    pick_delay, config->pick_config.snr_signal_window_samples,
                    max_lag_samples, LOCALIZATION_CALCULATE_XCORR_MAX_ADJUST_SAMPLES,
                    &refined_delay
                ) &&
                reference_position + refined_delay >= 0.0
            ) {
                const double position = reference_position + refined_delay;
                const double index = ceil(position);
                event_channels[ch].pick.arrival_sample_index = (size_t)index;
                event_channels[ch].pick.arrival_subsample_offset = (float)(position - index);
                event_channels[ch].pick.arrival_time_s = (float)(position / (double)sample_rate_hz);
            }
        }
    }

    bool any_accepted = false;
    size_t reference_sample_index = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED) {
            if (!any_accepted || event_channels[ch].pick.arrival_sample_index < reference_sample_index) {
                reference_sample_index = event_channels[ch].pick.arrival_sample_index;
            }
            any_accepted = true;
        }
    }

    /*
     * MAX-148 (slice 3): persist the per-channel pick evidence this run
     * actually used, versioned by the same computed_at_ms as the method
     * records below -- so a WebUI can explain *why* a channel was rejected
     * (no trigger / no refinement / low SNR) rather than only seeing the
     * aggregate channel_mask_rejected each run record carries. Best-effort:
     * pick evidence is diagnostic context for the runs below, not itself
     * part of the recording-gate's admission promise, so a persistence
     * failure here must never abort the whole calculate_source invocation.
     */
    localization_pick_record_t pick_record;
    memset(&pick_record, 0, sizeof(pick_record));
    snprintf(pick_record.object_id, sizeof(pick_record.object_id), "%s", object_id);
    snprintf(pick_record.session_id, sizeof(pick_record.session_id), "%s", session_id);
    pick_record.computed_at_ms = computed_at_ms;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        pick_record.channels[ch].participating = geometry.pods[ch].computed;
        pick_record.channels[ch].pick = event_channels[ch].pick;
    }
    (void)localization_run_store_write_picks(object_id, session_id, &pick_record);

    localization_point3d_t participant_positions[OBJECT_STORE_POD_COUNT];
    size_t participant_position_count = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (geometry.pods[ch].computed) {
            participant_positions[participant_position_count++] = event_channels[ch].sensor_position;
        }
    }

    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;
    localization_calculate_search_bounds(
        participant_positions, participant_position_count, dimensionality,
        config->max_source_depth_m, &semblance_bounds, &traveltime_bounds
    );

    const uint64_t geometry_computed_at_ms =
        localization_calculate_geometry_computed_at_ms(&geometry);

    localization_calculate_run_context_t context = {
        .object_id = object_id,
        .session_id = session_id,
        .computed_at_ms = computed_at_ms,
        .geometry_computed_at_ms = geometry_computed_at_ms,
        .dimensionality = dimensionality,
        .propagation_model_name = config->propagation_model_name,
        .propagation_speed_mps = config->propagation_speed_mps,
        .propagation_speed_uncertainty_mps = config->propagation_speed_uncertainty_mps,
    };

    localization_run_record_t records[4];
    size_t record_count = 0U;

    /* --- TDOA (Gauss-Newton), via the existing MAX-135 bridge ------------ */

    localization_config_t tdoa_config = localization_default_config();
    tdoa_config.propagation_speed_mps = config->propagation_speed_mps;

    localization_event_result_t event_result;
    localization_event_solve(
        event_channels, OBJECT_STORE_POD_COUNT, &tdoa_config, NULL, &event_result
    );
    summary_out->methods_attempted++;

    localization_event_result_to_run_record(
        &event_result, object_id, session_id, computed_at_ms, geometry_computed_at_ms,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->propagation_model_name,
        config->propagation_speed_mps, config->propagation_speed_uncertainty_mps,
        config->timing_uncertainty_us, &records[record_count]
    );
    /* MAX-212: the Gauss-Newton TDOA solve is always 3D. */
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(event_result.channels_used_count, false)
    );
    ++record_count;

    /* --- Statistical traveltime inversion (MAX-136) ---------------------- */

    localization_traveltime_pick_t traveltime_picks[OBJECT_STORE_POD_COUNT];
    uint32_t traveltime_used_mask = 0U;
    uint32_t traveltime_rejected_mask = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        const bool usable =
            geometry.pods[ch].computed &&
            event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED;

        traveltime_picks[ch].observation.sensor_position = event_channels[ch].sensor_position;
        traveltime_picks[ch].observation.arrival_time_us = usable
            ? localization_calculate_arrival_time_us_at(
                  localization_calculate_pick_sample_position(&event_channels[ch].pick),
                  session_summary.sample_rate_hz
              )
            : 0;
        traveltime_picks[ch].weight = usable ? 1.0 : 0.0;

        if (usable) {
            traveltime_used_mask |= (1U << ch);
        } else {
            traveltime_rejected_mask |= (1U << ch);
        }
    }

    const localization_traveltime_config_t traveltime_config = {
        .propagation_speed_mps = config->propagation_speed_mps,
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        /* MAX-208: streaming search within the default candidate budget; the
         * per-candidate surface is not persisted, so it is never built. */
        .max_coarse_candidates = 0U,
        .retain_surface = false,
    };
    localization_traveltime_result_t traveltime_result;
    memset(&traveltime_result, 0, sizeof(traveltime_result));
    const esp_err_t traveltime_err = localization_traveltime_search(
        traveltime_picks, OBJECT_STORE_POD_COUNT, &traveltime_bounds,
        &traveltime_config, &traveltime_result
    );
    summary_out->methods_attempted++;

    localization_calculate_traveltime_to_run_record(
        &context, traveltime_err == ESP_OK ? &traveltime_result : NULL,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(
            traveltime_result.picks_used_count,
            dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
        )
    );
    ++record_count;
    free(traveltime_result.surface);

    /* --- Semblance / coherency (MAX-137) ---------------------------------
     * Uses every participating channel's raw DC-removed window regardless
     * of that channel's own pick outcome (a coherency operator measures the
     * waveform shape directly), but can only run at all once some channel,
     * anywhere, has an accepted pick to anchor its search window on. */

    const bool semblance_attempted = any_accepted;
    localization_semblance_result_t semblance_result;
    memset(&semblance_result, 0, sizeof(semblance_result));
    esp_err_t semblance_err = ESP_OK;

    if (semblance_attempted) {
        localization_semblance_channel_t semblance_channels[OBJECT_STORE_POD_COUNT];
        size_t semblance_channel_count = 0U;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (geometry.pods[ch].computed) {
                semblance_channels[semblance_channel_count].samples = channel_samples[ch];
                semblance_channels[semblance_channel_count].sample_count = frame_count;
                semblance_channels[semblance_channel_count].sample_rate_hz = session_summary.sample_rate_hz;
                semblance_channels[semblance_channel_count].sensor_position = event_channels[ch].sensor_position;
                ++semblance_channel_count;
            }
        }

        const localization_semblance_config_t semblance_config = {
            .propagation_speed_mps = config->propagation_speed_mps,
            .reference_sample_index = reference_sample_index,
            .window_half_span_samples = config->semblance_window_half_span_samples,
            .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        };
        semblance_err = localization_semblance_search(
            semblance_channels, semblance_channel_count, &semblance_bounds,
            &semblance_config, &semblance_result
        );
        summary_out->methods_attempted++;
    }

    localization_calculate_semblance_to_run_record(
        &context, (semblance_attempted && semblance_err == ESP_OK) ? &semblance_result : NULL,
        semblance_attempted
            ? "semblance search call failed"
            : "no accepted arrival pick to anchor the search window",
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
    );
    ++record_count;

    /* --- Energy baseline (MAX-138 part 1) --------------------------------- */

    /* MAX-212: energy over the EVENT window (earliest accepted pick +
     * signal window + the array's largest moveout), not the whole recording,
     * which is dominated by background noise. Without an accepted pick there
     * is no event window, so the baseline is not attempted. */
    size_t energy_start = 0U;
    size_t energy_end = 0U;
    const bool energy_window_known =
        any_accepted &&
        localization_calculate_event_window(
            reference_sample_index, frame_count,
            config->pick_config.snr_signal_window_samples,
            max_sensor_distance_m, config->propagation_speed_mps,
            session_summary.sample_rate_hz, &energy_start, &energy_end
        );

    localization_energy_baseline_channel_t energy_channels[OBJECT_STORE_POD_COUNT];
    size_t energy_channel_count = 0U;
    for (size_t ch = 0U; energy_window_known && ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (geometry.pods[ch].computed && channel_samples[ch] != NULL) {
            double sum_squares = 0.0;
            for (size_t i = energy_start; i < energy_end; ++i) {
                sum_squares += (double)channel_samples[ch][i] * (double)channel_samples[ch][i];
            }
            energy_channels[energy_channel_count].sensor_position = event_channels[ch].sensor_position;
            energy_channels[energy_channel_count].energy =
                energy_end > energy_start ? sum_squares / (double)(energy_end - energy_start) : 0.0;
            energy_channels[energy_channel_count].weight = 1.0;
            ++energy_channel_count;
        }
    }

    localization_energy_baseline_result_t energy_result;
    memset(&energy_result, 0, sizeof(energy_result));
    esp_err_t energy_err = ESP_ERR_INVALID_STATE;
    if (energy_window_known) {
        energy_err = localization_energy_baseline_estimate(
            energy_channels, energy_channel_count, &energy_result
        );
        summary_out->methods_attempted++;
    }

    localization_calculate_energy_baseline_to_run_record(
        &context, energy_err == ESP_OK ? &energy_result : NULL,
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
    );
    if (!energy_window_known) {
        snprintf(
            records[record_count].rejection_reason,
            sizeof(records[record_count].rejection_reason), "%s",
            "no accepted arrival pick to define the event energy window"
        );
    }
    ++record_count;

    free_channel_buffers(channel_samples);

    for (size_t i = 0U; i < record_count; ++i) {
        if (records[i].solved) {
            summary_out->methods_solved++;
        }
    }

    /* --- Persist every individual method's own record, solved or not ----- */

    esp_err_t first_write_err = ESP_OK;
    for (size_t i = 0U; i < record_count; ++i) {
        const esp_err_t write_err = localization_run_store_write(object_id, session_id, &records[i]);
        if (write_err == ESP_OK) {
            summary_out->methods_persisted++;
        } else if (first_write_err == ESP_OK) {
            first_write_err = write_err;
        }
    }

    /* --- Joint quality-weighted fusion (MAX-138 part 4) ------------------- */

    localization_comparison_method_result_t comparison_methods[4];
    for (size_t i = 0U; i < record_count; ++i) {
        localization_calculate_run_record_to_comparison_method(&records[i], &comparison_methods[i]);
    }

    localization_comparison_result_t comparison_result;
    memset(&comparison_result, 0, sizeof(comparison_result));
    const esp_err_t comparison_err = localization_comparison_fuse(
        comparison_methods, record_count, &comparison_result
    );

    localization_run_record_t joint_record;
    localization_calculate_joint_to_run_record(
        &context, comparison_err == ESP_OK ? &comparison_result : NULL,
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, &joint_record
    );
    if (joint_record.solved) {
        summary_out->methods_solved++;
    }

    const esp_err_t joint_write_err = localization_run_store_write(object_id, session_id, &joint_record);
    if (joint_write_err == ESP_OK) {
        summary_out->methods_persisted++;
    } else if (first_write_err == ESP_OK) {
        first_write_err = joint_write_err;
    }

    return first_write_err;
}
