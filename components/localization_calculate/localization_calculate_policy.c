#include "localization_calculate_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


#define LOCALIZATION_CALCULATE_SEARCH_STEP_M 0.1
#define LOCALIZATION_CALCULATE_SEARCH_MARGIN_FRACTION 0.5
#define LOCALIZATION_CALCULATE_SEARCH_MIN_MARGIN_M 1.0


double localization_calculate_quality_from_residual_us(
    double residual_us,
    double timing_uncertainty_us
)
{
    if (
        !isfinite(residual_us) ||
        !isfinite(timing_uncertainty_us) ||
        timing_uncertainty_us <= 0.0
    ) {
        return 0.0;
    }

    const double ratio = residual_us / timing_uncertainty_us;
    const double quality = exp(-0.5 * ratio * ratio);
    return isfinite(quality) ? quality : 0.0;
}


size_t localization_calculate_participating_channels(
    const object_store_geometry_t *geometry,
    uint32_t *channel_mask_out
)
{
    size_t count = 0U;
    uint32_t mask = 0U;

    if (geometry != NULL) {
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            if (geometry->pods[i].computed) {
                mask |= (1U << i);
                ++count;
            }
        }
    }

    if (channel_mask_out != NULL) {
        *channel_mask_out = mask;
    }
    return count;
}


localization_point3d_t localization_calculate_sensor_position(
    const object_store_pod_geometry_t *pod
)
{
    localization_point3d_t position = { 0.0, 0.0, 0.0 };
    if (pod == NULL) {
        return position;
    }

    position.x_m = (double)pod->x_mm / 1000.0;
    position.y_m = (double)pod->y_mm / 1000.0;
    position.z_m = (double)pod->z_mm / 1000.0;
    return position;
}


uint64_t localization_calculate_geometry_computed_at_ms(
    const object_store_geometry_t *geometry
)
{
    uint64_t latest = 0ULL;
    if (geometry == NULL) {
        return latest;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (geometry->pods[i].computed && geometry->pods[i].computed_at_ms > latest) {
            latest = geometry->pods[i].computed_at_ms;
        }
    }
    return latest;
}


int64_t localization_calculate_arrival_time_us(
    size_t arrival_sample_index,
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U) {
        return 0;
    }

    const double arrival_time_us =
        (double)arrival_sample_index * 1.0e6 / (double)sample_rate_hz;
    return (int64_t)llround(arrival_time_us);
}


static esp_err_t rescale_sample_count(
    size_t *sample_count,
    uint32_t settings_rate_hz,
    uint32_t session_rate_hz
)
{
    const uint64_t count = (uint64_t)*sample_count;
    const uint64_t whole = count / settings_rate_hz;
    const uint64_t remainder = count % settings_rate_hz;
    if (whole > (uint64_t)SIZE_MAX / session_rate_hz) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t scaled_whole = whole * session_rate_hz;
    const uint64_t scaled_remainder =
        (remainder * session_rate_hz + settings_rate_hz / 2U) / settings_rate_hz;
    if (scaled_whole > (uint64_t)SIZE_MAX - scaled_remainder) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t scaled = (size_t)(scaled_whole + scaled_remainder);
    *sample_count = scaled == 0U ? 1U : scaled;
    return ESP_OK;
}


static bool valid_scaled_sta_lta(size_t sta, size_t lta, float trigger_ratio)
{
    return sta > 0U && lta > sta && isfinite(trigger_ratio) && trigger_ratio > 0.0f &&
        (double)trigger_ratio < (double)lta / (double)sta;
}


esp_err_t localization_calculate_rescale_sample_windows(
    localization_calculate_config_t *config,
    uint32_t session_sample_rate_hz,
    char *reason_out,
    size_t reason_size
)
{
    if (reason_out != NULL && reason_size > 0U) reason_out[0] = '\0';
    if (config == NULL || session_sample_rate_hz == 0U) {
        if (reason_out != NULL && reason_size > 0U) {
            (void)snprintf(reason_out, reason_size, "configuration must be present and session sample rate must be nonzero");
        }
        return ESP_ERR_INVALID_ARG;
    }

    localization_calculate_config_t scaled = *config;
    const uint32_t settings_rate_hz = scaled.sample_rate_hz == 0U
        ? 500U
        : scaled.sample_rate_hz;
    size_t *counts[] = {
        &scaled.pick_config.noise_window_samples,
        &scaled.pick_config.sta_window_samples,
        &scaled.pick_config.lta_window_samples,
        &scaled.pick_config.refine_lookback_samples,
        &scaled.pick_config.refine_lookahead_samples,
        &scaled.pick_config.snr_signal_window_samples,
        &scaled.semblance_window_half_span_samples,
    };
    if (scaled.s_pick_enabled) {
        size_t *s_counts[] = {
            &scaled.s_pick_config.min_separation_samples,
            &scaled.s_pick_config.max_separation_samples,
            &scaled.s_pick_config.sta_window_samples,
            &scaled.s_pick_config.lta_window_samples,
        };
        for (size_t i = 0U; i < sizeof(s_counts) / sizeof(s_counts[0]); ++i) {
            if (rescale_sample_count(s_counts[i], settings_rate_hz, session_sample_rate_hz) != ESP_OK) {
                if (reason_out != NULL && reason_size > 0U) {
                    (void)snprintf(reason_out, reason_size, "sample-window rescaling overflowed size_t");
                }
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }
    for (size_t i = 0U; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        if (rescale_sample_count(counts[i], settings_rate_hz, session_sample_rate_hz) != ESP_OK) {
            if (reason_out != NULL && reason_size > 0U) {
                (void)snprintf(reason_out, reason_size, "sample-window rescaling overflowed size_t");
            }
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (!valid_scaled_sta_lta(
            scaled.pick_config.sta_window_samples,
            scaled.pick_config.lta_window_samples,
            scaled.pick_config.sta_lta_trigger_ratio)) {
        if (reason_out != NULL && reason_size > 0U) {
            (void)snprintf(reason_out, reason_size, "rescaled P-wave STA/LTA windows violate the trigger ratio rule");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (scaled.s_pick_enabled && (
            scaled.s_pick_config.min_separation_samples >= scaled.s_pick_config.max_separation_samples ||
            !valid_scaled_sta_lta(
                scaled.s_pick_config.sta_window_samples,
                scaled.s_pick_config.lta_window_samples,
                scaled.s_pick_config.sta_lta_trigger_ratio))) {
        if (reason_out != NULL && reason_size > 0U) {
            (void)snprintf(reason_out, reason_size, "rescaled S-wave separation or STA/LTA windows are invalid");
        }
        return ESP_ERR_INVALID_ARG;
    }

    scaled.sample_rate_hz = session_sample_rate_hz;
    *config = scaled;
    return ESP_OK;
}


void localization_calculate_search_bounds(
    const localization_point3d_t *positions,
    size_t position_count,
    localization_dimensionality_t dimensionality,
    double max_source_depth_m,
    localization_semblance_search_bounds_t *semblance_bounds_out,
    localization_traveltime_search_bounds_t *traveltime_bounds_out
)
{
    double min_x = 0.0, max_x = 0.0;
    double min_y = 0.0, max_y = 0.0;
    double min_z = 0.0, max_z = 0.0;

    if (positions != NULL && position_count > 0U) {
        min_x = max_x = positions[0].x_m;
        min_y = max_y = positions[0].y_m;
        min_z = max_z = positions[0].z_m;
        for (size_t i = 1U; i < position_count; ++i) {
            if (positions[i].x_m < min_x) min_x = positions[i].x_m;
            if (positions[i].x_m > max_x) max_x = positions[i].x_m;
            if (positions[i].y_m < min_y) min_y = positions[i].y_m;
            if (positions[i].y_m > max_y) max_y = positions[i].y_m;
            if (positions[i].z_m < min_z) min_z = positions[i].z_m;
            if (positions[i].z_m > max_z) max_z = positions[i].z_m;
        }
    }

    const double span_x = max_x - min_x;
    const double span_y = max_y - min_y;
    const double diagonal = sqrt(span_x * span_x + span_y * span_y);
    double margin = diagonal * LOCALIZATION_CALCULATE_SEARCH_MARGIN_FRACTION;
    if (margin < LOCALIZATION_CALCULATE_SEARCH_MIN_MARGIN_M) {
        margin = LOCALIZATION_CALCULATE_SEARCH_MIN_MARGIN_M;
    }

    const double mean_z = (min_z + max_z) / 2.0;
    const bool planar = dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D;
    const double depth =
        isfinite(max_source_depth_m) && max_source_depth_m > 0.0
        ? max_source_depth_m
        : margin;

    /* MAX-211: the search never goes above the highest sensor (the surface
     * the geophones lie on); it only goes down, by `depth`. */
    const localization_point3d_t min_corner = {
        min_x - margin,
        min_y - margin,
        planar ? mean_z : min_z - depth,
    };
    const localization_point3d_t max_corner = {
        max_x + margin,
        max_y + margin,
        planar ? mean_z : max_z,
    };

    if (semblance_bounds_out != NULL) {
        semblance_bounds_out->min_corner = min_corner;
        semblance_bounds_out->max_corner = max_corner;
        semblance_bounds_out->step_m = LOCALIZATION_CALCULATE_SEARCH_STEP_M;
    }
    if (traveltime_bounds_out != NULL) {
        traveltime_bounds_out->min_corner = min_corner;
        traveltime_bounds_out->max_corner = max_corner;
        traveltime_bounds_out->step_m = LOCALIZATION_CALCULATE_SEARCH_STEP_M;
    }
}


static void fill_common_fields(
    const localization_calculate_run_context_t *context,
    localization_run_kind_t kind,
    const char *method_name,
    uint32_t method_version,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    localization_run_record_t *record_out
)
{
    memset(record_out, 0, sizeof(*record_out));

    record_out->schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    snprintf(record_out->object_id, sizeof(record_out->object_id), "%s", context->object_id);
    snprintf(record_out->session_id, sizeof(record_out->session_id), "%s", context->session_id);
    record_out->computed_at_ms = context->computed_at_ms;
    record_out->kind = kind;
    snprintf(record_out->method_name, sizeof(record_out->method_name), "%s", method_name);
    record_out->method_version = method_version;
    record_out->channel_mask_used = channel_mask_used;
    record_out->channel_mask_rejected = channel_mask_rejected;
    record_out->dimensionality = context->dimensionality;
    record_out->geometry_computed_at_ms = context->geometry_computed_at_ms;
    snprintf(
        record_out->propagation_model_name,
        sizeof(record_out->propagation_model_name),
        "%s",
        context->propagation_model_name != NULL ? context->propagation_model_name : ""
    );
    snprintf(record_out->material, sizeof(record_out->material), "%s",
        context->material != NULL ? context->material : "");
    record_out->propagation_speed_mps = context->propagation_speed_mps;
    record_out->propagation_speed_uncertainty_mps = context->propagation_speed_uncertainty_mps;
    record_out->degrees_of_freedom = -1;
}


void localization_calculate_traveltime_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_traveltime_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "traveltime_inversion",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL) {
        localization_calculate_set_rejection(
            record_out, "traveltime inversion call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
        return;
    }
    if (result->status != LOCALIZATION_TRAVELTIME_SOLVED) {
        const bool scan_excluded = result->status == LOCALIZATION_TRAVELTIME_NO_ALLOWED_CANDIDATE;
        localization_calculate_set_rejection(
            record_out,
            scan_excluded
                ? "scan occupancy excluded every candidate (verified FREE air or outside the scan)"
                : "insufficient accepted picks for traveltime inversion",
            scan_excluded ? LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
        );
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->best_position;
    record_out->rms_residual_us = result->best_residual_rms_us;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->best_residual_rms_us, timing_uncertainty_us
    );
}


void localization_calculate_semblance_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_semblance_result_t *result,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "semblance",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL) {
        localization_calculate_set_rejection(
            record_out,
            (not_attempted_reason != NULL && not_attempted_reason[0] != '\0')
                ? not_attempted_reason
                : "semblance search was not attempted",
            not_attempted_failure_class
        );
        return;
    }

    if (!result->found) {
        localization_calculate_set_rejection(
            record_out, "semblance search found no coherent candidate", LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
        );
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->best_position;
    record_out->confidence_score = result->best_semblance;
}


void localization_calculate_energy_baseline_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_energy_baseline_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "energy_baseline",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL) {
        localization_calculate_set_rejection(
            record_out, "energy baseline call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
        return;
    }
    if (!result->found) {
        localization_calculate_set_rejection(
            record_out, "no channel had positive weighted energy", LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
        );
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->confidence_score = result->energy_concentration;
}


void localization_calculate_joint_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_comparison_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_JOINT_FUSION, "joint_fusion",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL || !result->joint_found) {
        localization_calculate_set_rejection(
            record_out, "no individual method solved with usable quality", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->joint_position;
    /* joint_total_quality is a SUM of contributing weights (can exceed 1);
     * confidence_score's own contract is [0,1], so this records the mean
     * contributing quality instead -- every input quality is itself already
     * bounded in [0,1] (see each individual adapter above), so the mean
     * stays in range too. */
    record_out->confidence_score = result->joint_methods_used_count > 0U
        ? result->joint_total_quality / (double)result->joint_methods_used_count
        : 0.0;
}


void localization_calculate_run_record_to_comparison_method(
    const localization_run_record_t *record,
    localization_comparison_method_result_t *method_out
)
{
    memset(method_out, 0, sizeof(*method_out));
    snprintf(method_out->method_name, sizeof(method_out->method_name), "%s", record->method_name);
    method_out->solved = record->solved;

    if (record->solved) {
        method_out->position = record->source_position;
        method_out->quality = record->confidence_score;
        /* MAX-212: TDOA, traveltime inversion, the velocity-free methods
         * and the extended ones all fit the same P arrivals -- correlated,
         * not independent, evidence (PSAFUVS / PSP add S picks but share
         * the P picks; the cross-correlation delays are the same onsets
         * measured from the waveforms). */
        if (
            strcmp(record->method_name, "tdoa_gauss_newton") == 0 ||
            strcmp(record->method_name, "traveltime_inversion") == 0 ||
            strcmp(record->method_name, "tdcas") == 0 ||
            strcmp(record->method_name, "psafuvs") == 0 ||
            strcmp(record->method_name, "suea") == 0 ||
            strcmp(record->method_name, "xcorr_tdoa") == 0 ||
            strcmp(record->method_name, "robust_tdoa") == 0 ||
            strcmp(record->method_name, "psp_joint") == 0 ||
            strcmp(record->method_name, "eikonal_fmm") == 0 ||
            strcmp(record->method_name, "bayesian") == 0
        ) {
            method_out->evidence_group = LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP;
        }
        /* The energy baseline is a sensor-weighted centroid that can never
         * leave the array's convex hull and whose "concentration" is >= 1/N
         * even for pure noise: diagnostic only, never a fusion weight. */
        if (strcmp(record->method_name, "energy_baseline") == 0) {
            method_out->quality = 0.0;
        }
    }
    else {
        /* localization_comparison's rejection_reason field (96 bytes) is
         * smaller than localization_run_record's (128 bytes); an explicit
         * precision bounds the copy to a length the compiler can prove
         * fits, rather than relying on snprintf's own runtime clamping. */
        snprintf(
            method_out->rejection_reason, sizeof(method_out->rejection_reason), "%.*s",
            (int)(sizeof(method_out->rejection_reason) - 1U), record->rejection_reason
        );
    }
}


int32_t localization_calculate_degrees_of_freedom(
    size_t picks_used,
    bool planar
)
{
    if (picks_used < 2U) {
        return -1;
    }
    return (int32_t)(picks_used - 1U) - (planar ? 2 : 3);
}


void localization_calculate_apply_redundancy(
    localization_run_record_t *record,
    int32_t degrees_of_freedom
)
{
    if (record == NULL) {
        return;
    }
    record->degrees_of_freedom = degrees_of_freedom;
    if (
        record->solved &&
        degrees_of_freedom <= 0 &&
        record->confidence_score > LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP
    ) {
        record->confidence_score = LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP;
    }
}


bool localization_calculate_event_window(
    size_t reference_sample_index,
    size_t frame_count,
    size_t signal_window_samples,
    double max_sensor_distance_m,
    double propagation_speed_mps,
    uint32_t sample_rate_hz,
    size_t *start_out,
    size_t *end_out
)
{
    if (
        start_out == NULL ||
        end_out == NULL ||
        reference_sample_index >= frame_count ||
        !(propagation_speed_mps > 0.0) ||
        !isfinite(max_sensor_distance_m) ||
        max_sensor_distance_m < 0.0 ||
        sample_rate_hz == 0U
    ) {
        return false;
    }

    const double moveout = ceil(
        max_sensor_distance_m / propagation_speed_mps * (double)sample_rate_hz
    );
    const double length = (double)signal_window_samples + moveout + 1.0;
    const double remaining = (double)(frame_count - reference_sample_index);

    *start_out = reference_sample_index;
    *end_out = length >= remaining
        ? frame_count
        : reference_sample_index + (size_t)length;
    return true;
}


double localization_calculate_pick_sample_position(
    const geophone_channel_pick_t *pick
)
{
    if (pick == NULL) {
        return 0.0;
    }
    return (double)pick->arrival_sample_index + (double)pick->arrival_subsample_offset;
}


int64_t localization_calculate_arrival_time_us_at(
    double sample_position,
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U || !isfinite(sample_position) || sample_position < 0.0) {
        return 0;
    }
    return (int64_t)llround(sample_position * 1.0e6 / (double)sample_rate_hz);
}


static bool picks_compatible(
    const double *arrival_time_s,
    const localization_point3d_t *positions,
    size_t i,
    size_t j,
    double v_min,
    double tolerance_s
)
{
    const double dx = positions[i].x_m - positions[j].x_m;
    const double dy = positions[i].y_m - positions[j].y_m;
    const double dz = positions[i].z_m - positions[j].z_m;
    const double max_dt = sqrt(dx * dx + dy * dy + dz * dz) / v_min + tolerance_s;
    return fabs(arrival_time_s[i] - arrival_time_s[j]) <= max_dt;
}


uint32_t localization_calculate_consistent_pick_mask(
    const double *arrival_time_s,
    const localization_point3d_t *positions,
    size_t channel_count,
    uint32_t candidate_mask,
    double propagation_speed_mps,
    double propagation_speed_uncertainty_mps,
    double tolerance_s
)
{
    if (
        arrival_time_s == NULL ||
        positions == NULL ||
        channel_count == 0U ||
        channel_count > 16U ||
        !(propagation_speed_mps > 0.0) ||
        !(tolerance_s >= 0.0)
    ) {
        return 0U;
    }

    double v_min = propagation_speed_mps;
    if (
        isfinite(propagation_speed_uncertainty_mps) &&
        propagation_speed_uncertainty_mps > 0.0 &&
        propagation_speed_mps - propagation_speed_uncertainty_mps > 0.0
    ) {
        v_min = propagation_speed_mps - propagation_speed_uncertainty_mps;
    }

    const uint32_t all = (1U << channel_count) - 1U;
    candidate_mask &= all;

    uint32_t best_mask = 0U;
    unsigned best_count = 0U;
    double best_span = 0.0;

    for (uint32_t subset = candidate_mask; ; subset = (subset - 1U) & candidate_mask) {
        unsigned count = 0U;
        bool consistent = true;
        double earliest = 0.0;
        double latest = 0.0;
        for (size_t i = 0U; consistent && i < channel_count; ++i) {
            if ((subset & (1U << i)) == 0U) {
                continue;
            }
            if (count == 0U || arrival_time_s[i] < earliest) earliest = arrival_time_s[i];
            if (count == 0U || arrival_time_s[i] > latest) latest = arrival_time_s[i];
            ++count;
            for (size_t j = i + 1U; j < channel_count; ++j) {
                if (
                    (subset & (1U << j)) != 0U &&
                    !picks_compatible(arrival_time_s, positions, i, j, v_min, tolerance_s)
                ) {
                    consistent = false;
                    break;
                }
            }
        }

        if (consistent && count > 0U) {
            const double span = latest - earliest;
            if (
                count > best_count ||
                (count == best_count && span < best_span) ||
                (count == best_count && span == best_span && subset < best_mask)
            ) {
                best_mask = subset;
                best_count = count;
                best_span = span;
            }
        }

        if (subset == 0U) {
            break;
        }
    }

    return best_mask;
}


bool localization_calculate_xcorr_relative_delay(
    const float *reference,
    const float *channel,
    size_t sample_count,
    size_t reference_pick_index,
    double pick_delay_samples,
    size_t window_samples,
    size_t max_lag_samples,
    double max_adjust_samples,
    double *delay_samples_out
)
{
    if (
        reference == NULL ||
        channel == NULL ||
        delay_samples_out == NULL ||
        window_samples == 0U ||
        reference_pick_index >= sample_count ||
        !isfinite(pick_delay_samples) ||
        !(max_adjust_samples >= 0.0)
    ) {
        return false;
    }

    const size_t start =
        reference_pick_index > window_samples / 2U ? reference_pick_index - window_samples / 2U : 0U;
    size_t length = window_samples + max_lag_samples;
    if (length > sample_count - start) {
        length = sample_count - start;
    }
    if (max_lag_samples >= length) {
        return false;
    }

    const geophone_cross_correlation_config_t config = { .max_lag_samples = max_lag_samples };
    geophone_cross_correlation_result_t result;
    if (
        !geophone_event_cross_correlate(
            reference + start, channel + start, length, &config, &result
        ) ||
        !result.found ||
        (double)result.peak_correlation < LOCALIZATION_CALCULATE_XCORR_MIN_CORRELATION ||
        fabs(result.delay_refined_samples - pick_delay_samples) > max_adjust_samples
    ) {
        return false;
    }

    *delay_samples_out = result.delay_refined_samples;
    return true;
}


bool localization_calculate_scan_candidate_allowed(
    const localization_point3d_t *candidate,
    void *context
)
{
    const scan_occupancy_t *occupancy = (const scan_occupancy_t *)context;
    if (candidate == NULL || occupancy == NULL) {
        return true;
    }
    const double sensor[3] = { candidate->x_m, candidate->y_m, candidate->z_m };
    double scan[3];
    scan_occupancy_sensor_to_scan(occupancy, sensor, scan);
    const scan_occupancy_class_t cls = scan_occupancy_classify(occupancy, scan);
    return cls == SCAN_OCCUPANCY_UNKNOWN || cls == SCAN_OCCUPANCY_OCCUPIED;
}



void localization_calculate_tdcas_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_tdcas_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "tdcas",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL || result->status != LOCALIZATION_TDCAS_SOLVED) {
        if (result != NULL && result->status == LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS) {
            char reason[LOCALIZATION_RUN_REJECTION_REASON_SIZE];
            snprintf(
                reason, sizeof(reason),
                "TDCAS needs %u accepted P picks for this layout (%s), have %u",
                (unsigned)(result->planar_layout ? LOCALIZATION_TDCAS_MIN_SENSORS_PLANAR : LOCALIZATION_TDCAS_MIN_SENSORS_3D),
                result->planar_layout ? "planar surface array" : "depth-resolving array",
                (unsigned)result->sensors_used
            );
            localization_calculate_set_rejection(
                record_out, reason, LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
            );
        } else {
            const bool scan_excluded = result != NULL && result->status == LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION;
            localization_calculate_set_rejection(
                record_out,
                result == NULL
                    ? "TDCAS was not attempted"
                    : scan_excluded
                        ? "scan occupancy excluded every sensor combination (FREE air or obstructed rays)"
                        : "no sensor combination gave a physical velocity-free solution",
                result == NULL
                    ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
                    : LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
            );
        }
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_residual_us;
    record_out->estimated_p_velocity_mps = result->velocity_mps;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_residual_us, timing_uncertainty_us
    );
}


void localization_calculate_psafuvs_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_psafuvs_result_t *result,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "psafuvs",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL || result->status != LOCALIZATION_PSAFUVS_SOLVED) {
        record_out->solved = false;
        const char *reason = "PSAFUVS was not attempted";
        localization_failure_class_t failure_class = not_attempted_failure_class;
        if (result == NULL) {
            if (not_attempted_reason != NULL && not_attempted_reason[0] != '\0') {
                reason = not_attempted_reason;
            }
        } else {
            switch (result->status) {
                case LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    reason = result->planar_layout
                        ? "PSAFUVS needs 4 channels with accepted P and S picks on a planar array"
                        : "PSAFUVS needs 5 channels with accepted P and S picks on a depth-resolving array";
                    break;
                case LOCALIZATION_PSAFUVS_NONPHYSICAL_VELOCITIES:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    reason = "P/S fit is not physical (S velocity not below P velocity)";
                    break;
                case LOCALIZATION_PSAFUVS_RAYS_OBSTRUCTED:
                    failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
                    reason = "scan voids obstruct too many straight rays for PSAFUVS";
                    break;
                case LOCALIZATION_PSAFUVS_NO_ALLOWED_CANDIDATE:
                    failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
                    reason = "PSAFUVS solution lies in scan-verified FREE air or outside the scan";
                    break;
                case LOCALIZATION_PSAFUVS_DEGENERATE:
                default:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    reason = "P and S arrivals do not describe one source (degenerate system)";
                    break;
            }
        }
        localization_calculate_set_rejection(record_out, reason, failure_class);
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_residual_us;
    record_out->iterations = result->iterations;
    record_out->converged = result->converged;
    record_out->estimated_p_velocity_mps = result->p_velocity_mps;
    record_out->estimated_s_velocity_mps = result->s_velocity_mps;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_residual_us, timing_uncertainty_us
    );
}


void localization_calculate_suea_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_suea_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "suea",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );

    if (result == NULL || result->status != LOCALIZATION_SUEA_SOLVED) {
        const bool unreachable_sensor =
            result != NULL && result->status == LOCALIZATION_SUEA_INSUFFICIENT_PICKS &&
            result->pick_mask_unreachable != 0U;
        const localization_failure_class_t failure_class = result == NULL ||
            (result->status == LOCALIZATION_SUEA_INSUFFICIENT_PICKS && !unreachable_sensor)
            ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
            : LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
        localization_calculate_set_rejection(
            record_out,
            result == NULL
                ? "SUEA was not attempted"
                : result->status == LOCALIZATION_SUEA_INSUFFICIENT_PICKS
                    ? (unreachable_sensor
                        ? "fewer than 3 usable picks: some sensors sit inside scan-verified empty space"
                        : "fewer than 3 accepted picks for SUEA")
                    : result->status == LOCALIZATION_SUEA_NO_ALLOWED_CANDIDATE
                        ? "scan occupancy excluded every reachable grid point"
                        : "no grid point is reachable from every sensor with a physical fit",
            failure_class
        );
        return;
    }

    record_out->solved = true;
    record_out->source_position = result->best_position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->best_residual_rms_us;
    record_out->estimated_p_velocity_mps = result->velocity_mps;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->best_residual_rms_us, timing_uncertainty_us
    );
}


void localization_calculate_suea_grid_bounds(
    const localization_traveltime_search_bounds_t *search,
    localization_point3d_t *min_out,
    localization_point3d_t *max_out
)
{
    if (search == NULL || min_out == NULL || max_out == NULL) {
        return;
    }
    const double margin = search->step_m > 0.0 ? search->step_m : 0.0;
    const bool planar = search->min_corner.z_m == search->max_corner.z_m;
    *min_out = (localization_point3d_t) {
        search->min_corner.x_m - margin,
        search->min_corner.y_m - margin,
        search->min_corner.z_m,
    };
    *max_out = (localization_point3d_t) {
        search->max_corner.x_m + margin,
        search->max_corner.y_m + margin,
        planar ? search->max_corner.z_m : search->max_corner.z_m + margin,
    };
}


int32_t localization_calculate_velocity_free_degrees_of_freedom(
    size_t observations,
    size_t unknowns
)
{
    if (observations == 0U) {
        return -1;
    }
    return (int32_t)observations - (int32_t)unknowns;
}


localization_suea_cell_t localization_calculate_scan_suea_cell(
    const localization_point3d_t *point,
    void *context
)
{
    const scan_occupancy_t *occupancy = (const scan_occupancy_t *)context;
    if (point == NULL || occupancy == NULL) {
        return LOCALIZATION_SUEA_CELL_UNKNOWN;
    }
    const double sensor[3] = { point->x_m, point->y_m, point->z_m };
    double scan[3];
    scan_occupancy_sensor_to_scan(occupancy, sensor, scan);
    switch (scan_occupancy_classify(occupancy, scan)) {
        case SCAN_OCCUPANCY_FREE:
            return LOCALIZATION_SUEA_CELL_EMPTY;
        case SCAN_OCCUPANCY_OCCUPIED:
            return LOCALIZATION_SUEA_CELL_SOLID;
        case SCAN_OCCUPANCY_OUTSIDE:
            return occupancy->has_envelope && scan[2] > occupancy->envelope_max[2]
                ? LOCALIZATION_SUEA_CELL_EMPTY
                : LOCALIZATION_SUEA_CELL_UNKNOWN;
        case SCAN_OCCUPANCY_UNKNOWN:
        default:
            return LOCALIZATION_SUEA_CELL_UNKNOWN;
    }
}


bool localization_calculate_scan_ray_clear(
    const localization_point3d_t *sensor,
    const localization_point3d_t *candidate,
    void *context
)
{
    const localization_calculate_scan_ray_context_t *ray =
        (const localization_calculate_scan_ray_context_t *)context;
    if (
        sensor == NULL || candidate == NULL || ray == NULL || ray->occupancy == NULL ||
        !(ray->occupancy->voxel_size_m > 0.0)
    ) {
        return true;
    }

    const double dx = candidate->x_m - sensor->x_m;
    const double dy = candidate->y_m - sensor->y_m;
    const double dz = candidate->z_m - sensor->z_m;
    const double length = sqrt(dx * dx + dy * dy + dz * dz);
    if (!isfinite(length)) {
        return true;
    }
    const double step = 0.5 * ray->occupancy->voxel_size_m;
    const double clearance = ray->sensor_clearance_m > 0.0 ? ray->sensor_clearance_m : 0.0;

    unsigned free_run = 0U;
    for (double t = clearance; t <= length; t += step) {
        const double f = length > 0.0 ? t / length : 0.0;
        const double point[3] = { sensor->x_m + f * dx, sensor->y_m + f * dy, sensor->z_m + f * dz };
        double scan[3];
        scan_occupancy_sensor_to_scan(ray->occupancy, point, scan);
        if (scan_occupancy_classify(ray->occupancy, scan) == SCAN_OCCUPANCY_FREE) {
            if (++free_run >= 2U) {
                return false;
            }
        } else {
            free_run = 0U;
        }
    }
    return true;
}


/* ---- localization_extended ------------------------------------------ */

localization_extended_bounds_t localization_calculate_extended_bounds(
    const localization_traveltime_search_bounds_t *search
)
{
    localization_extended_bounds_t bounds;
    memset(&bounds, 0, sizeof(bounds));
    if (search != NULL) {
        bounds.min_corner = search->min_corner;
        bounds.max_corner = search->max_corner;
        bounds.step_m = search->step_m;
    }
    return bounds;
}


void localization_calculate_set_rejection(
    localization_run_record_t *record,
    const char *reason,
    localization_failure_class_t failure_class
)
{
    if (record == NULL) {
        return;
    }
    record->solved = false;
    record->uncertainty_too_high = false;
    record->failure_class = failure_class;
    snprintf(record->rejection_reason, sizeof(record->rejection_reason), "%s", reason);
}


void localization_calculate_recommendation(
    const localization_run_record_t *records,
    size_t record_count,
    const localization_pick_record_t *pick_records,
    size_t pick_record_count,
    localization_recommendation_t *recommendation_out
)
{
    if (recommendation_out == NULL) {
        return;
    }
    *recommendation_out = (localization_recommendation_t) { 0 };

    uint64_t latest_computed_at_ms = 0U;
    bool have_methods = false;
    for (size_t i = 0U; records != NULL && i < record_count; ++i) {
        if (records[i].kind == LOCALIZATION_RUN_KIND_JOINT_FUSION) {
            continue;
        }
        if (!have_methods || records[i].computed_at_ms > latest_computed_at_ms) {
            latest_computed_at_ms = records[i].computed_at_ms;
        }
        have_methods = true;
    }

    size_t record_again_count = 0U;
    size_t move_sensors_count = 0U;
    bool have_usable_result = false;
    for (size_t i = 0U; records != NULL && i < record_count; ++i) {
        const localization_run_record_t *record = &records[i];
        if (
            record->kind == LOCALIZATION_RUN_KIND_JOINT_FUSION ||
            record->computed_at_ms != latest_computed_at_ms
        ) {
            continue;
        }
        if (record->solved && !record->uncertainty_too_high) {
            have_usable_result = true;
            continue;
        }
        if (record->uncertainty_too_high || record->failure_class == LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS) {
            ++move_sensors_count;
        } else if (record->failure_class == LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN) {
            ++record_again_count;
        }
    }

    if (have_usable_result) {
        recommendation_out->successful = true;
        snprintf(
            recommendation_out->reason_summary,
            sizeof(recommendation_out->reason_summary),
            "%s",
            "At least one method produced a usable result."
        );
        return;
    }

    const localization_pick_record_t *latest_picks = NULL;
    for (size_t i = 0U; pick_records != NULL && i < pick_record_count; ++i) {
        if (pick_records[i].computed_at_ms == latest_computed_at_ms) {
            latest_picks = &pick_records[i];
            break;
        }
    }
    size_t participating_count = 0U;
    size_t no_trigger_or_low_snr_count = 0U;
    if (latest_picks != NULL) {
        for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
            const localization_pick_channel_t *channel = &latest_picks->channels[i];
            if (!channel->participating) {
                continue;
            }
            ++participating_count;
            if (
                channel->pick.status == GEOPHONE_PICK_REJECTED_NO_TRIGGER ||
                channel->pick.status == GEOPHONE_PICK_REJECTED_LOW_SNR
            ) {
                ++no_trigger_or_low_snr_count;
            }
        }
    }
    if (participating_count > 0U && no_trigger_or_low_snr_count * 2U > participating_count) {
        recommendation_out->action = LOCALIZATION_RECOMMENDATION_RECORD_AGAIN;
        snprintf(
            recommendation_out->reason_summary,
            sizeof(recommendation_out->reason_summary),
            "%s",
            "Most participating channels had no trigger or low SNR."
        );
        return;
    }

    if (record_again_count == 0U && move_sensors_count == 0U) {
        snprintf(
            recommendation_out->reason_summary,
            sizeof(recommendation_out->reason_summary),
            "%s",
            have_methods ? "Only internal failures were reported; retry calculation."
                          : "No localization method result is available."
        );
    } else if (record_again_count >= move_sensors_count) {
        recommendation_out->action = LOCALIZATION_RECOMMENDATION_RECORD_AGAIN;
        snprintf(
            recommendation_out->reason_summary,
            sizeof(recommendation_out->reason_summary),
            "Method failures favor recording again (%u pick-related, %u geometry-related).",
            (unsigned)record_again_count,
            (unsigned)move_sensors_count
        );
    } else {
        recommendation_out->action = LOCALIZATION_RECOMMENDATION_MOVE_SENSORS;
        snprintf(
            recommendation_out->reason_summary,
            sizeof(recommendation_out->reason_summary),
            "Method failures favor moving sensors (%u geometry-related, %u pick-related).",
            (unsigned)move_sensors_count,
            (unsigned)record_again_count
        );
    }
}


void localization_calculate_xcorr_tdoa_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_xcorr_tdoa_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "xcorr_tdoa",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );
    if (result == NULL || result->status != LOCALIZATION_XCORR_TDOA_SOLVED) {
        if (result != NULL && result->status == LOCALIZATION_XCORR_TDOA_INSUFFICIENT_PAIRS) {
            char reason[LOCALIZATION_RUN_REJECTION_REASON_SIZE];
            snprintf(
                reason, sizeof(reason),
                "only %u of %u channel pairs correlate (>= %.1f) across %u channels: too few for a %s fit",
                (unsigned)result->pairs_measured, (unsigned)result->pairs_total,
                LOCALIZATION_CALCULATE_XCORR_MIN_CORRELATION, (unsigned)result->channels_used,
                result->planar ? "planar" : "3D"
            );
            localization_calculate_set_rejection(
                record_out, reason, LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
            );
            return;
        }
        localization_calculate_set_rejection(
            record_out,
            result == NULL
                ? "cross-correlation TDOA call failed"
                : result->status == LOCALIZATION_XCORR_TDOA_NO_ANCHOR
                    ? "no accepted arrival pick to anchor the correlation windows"
                    : result->status == LOCALIZATION_XCORR_TDOA_NO_ALLOWED_CANDIDATE
                        ? "scan occupancy excluded every candidate"
                        : "pair delays do not determine the source (underdetermined geometry)",
            result == NULL
                ? LOCALIZATION_FAILURE_CLASS_INTERNAL
                : result->status == LOCALIZATION_XCORR_TDOA_NO_ANCHOR
                    ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
                    : LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
        );
        return;
    }
    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->rms_residual_us = result->rms_pair_residual_us;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_pair_residual_us, timing_uncertainty_us
    );
}


void localization_calculate_robust_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_robust_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "robust_tdoa",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );
    if (result == NULL || result->status != LOCALIZATION_ROBUST_SOLVED) {
        if (result != NULL && result->status == LOCALIZATION_ROBUST_INSUFFICIENT_OBSERVATIONS) {
            char reason[LOCALIZATION_RUN_REJECTION_REASON_SIZE];
            snprintf(
                reason, sizeof(reason),
                "robust fit needs %u accepted P picks for a %s search, have %u",
                result->planar ? 3U : 4U, result->planar ? "planar" : "3D",
                (unsigned)(result->degrees_of_freedom + (result->planar ? 3 : 4))
            );
            localization_calculate_set_rejection(
                record_out, reason, LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
            );
            return;
        }
        localization_calculate_set_rejection(
            record_out,
            result == NULL
                ? "robust fit was not attempted"
                : "scan occupancy excluded every candidate",
            result == NULL
                ? LOCALIZATION_FAILURE_CLASS_INTERNAL
                : LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
        );
        return;
    }
    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_residual_us;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_residual_us, timing_uncertainty_us
    );
    if (result->consensus_ambiguous && record_out->confidence_score > LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP) {
        record_out->confidence_score = LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP;
    }
}


void localization_calculate_psp_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_psp_result_t *result,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, "psp_joint",
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );
    if (result == NULL || result->status != LOCALIZATION_PSP_SOLVED) {
        const char *reason = not_attempted_reason != NULL && not_attempted_reason[0] != '\0'
            ? not_attempted_reason
            : "joint P-TDOA + P-S fit was not attempted";
        localization_failure_class_t failure_class = not_attempted_failure_class;
        if (result != NULL) {
            switch (result->status) {
                case LOCALIZATION_PSP_INSUFFICIENT_OBSERVATIONS:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    reason = "too few P picks and S-P intervals for the joint fit's unknowns";
                    break;
                case LOCALIZATION_PSP_NO_ALLOWED_CANDIDATE:
                    failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
                    reason = "scan occupancy excluded every candidate";
                    break;
                case LOCALIZATION_PSP_NONPHYSICAL_VELOCITIES:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    reason = "no candidate gives physical velocities (vS < vP)";
                    break;
                case LOCALIZATION_PSP_UNDERDETERMINED:
                    failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
                    reason = "underdetermined: pods on one circle, depth trades against vP (set a vP uncertainty or move a pod)";
                    break;
                case LOCALIZATION_PSP_NO_S_PICK:
                default:
                    failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
                    break;
            }
        }
        localization_calculate_set_rejection(record_out, reason, failure_class);
        return;
    }
    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_p_residual_us;
    record_out->estimated_p_velocity_mps = result->p_velocity_mps;
    record_out->estimated_s_velocity_mps = result->s_velocity_mps;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_p_residual_us, timing_uncertainty_us
    );
}


void localization_calculate_eikonal_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_eikonal_result_t *result,
    const char *method_name,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    bool velocity_estimated,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, method_name,
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );
    if (result == NULL || result->status != LOCALIZATION_EIKONAL_SOLVED) {
        const char *reason = not_attempted_reason != NULL && not_attempted_reason[0] != '\0'
            ? not_attempted_reason
            : "eikonal location was not attempted";
        localization_failure_class_t failure_class = not_attempted_failure_class;
        if (result != NULL) {
            if (result->status == LOCALIZATION_EIKONAL_INSUFFICIENT_PICKS) {
                reason = "too few picked sensors are reachable in the velocity model";
                failure_class = result->pick_mask_unreachable != 0U
                    ? LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
                    : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
            } else if (result->status == LOCALIZATION_EIKONAL_NO_REACHABLE_CANDIDATE) {
                reason = "no candidate reaches every picked sensor around the voids";
                failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
            } else {
                reason = "scan occupancy excluded every candidate";
                failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
            }
        }
        localization_calculate_set_rejection(record_out, reason, failure_class);
        return;
    }
    record_out->solved = true;
    record_out->source_position = result->position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_residual_us;
    if (velocity_estimated && result->velocity_scale > 0.0 && context->propagation_speed_mps > 0.0) {
        record_out->estimated_p_velocity_mps = context->propagation_speed_mps / result->velocity_scale;
    }
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_residual_us, timing_uncertainty_us
    );
}


void localization_calculate_bayes_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_bayes_result_t *result,
    const char *method_name,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    bool velocity_estimated,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    fill_common_fields(
        context, LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD, method_name,
        method_version, channel_mask_used, channel_mask_rejected, record_out
    );
    if (result == NULL || result->status != LOCALIZATION_BAYES_SOLVED) {
        const char *reason = not_attempted_reason != NULL && not_attempted_reason[0] != '\0'
            ? not_attempted_reason
            : "Bayesian location was not attempted";
        localization_failure_class_t failure_class = not_attempted_failure_class;
        if (result != NULL) {
            if (result->status == LOCALIZATION_BAYES_INSUFFICIENT_PICKS) {
                reason = "too few accepted P picks for the posterior";
                failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
            } else if (result->status == LOCALIZATION_BAYES_NO_REACHABLE_CANDIDATE) {
                reason = "no candidate reaches every picked sensor around the voids";
                failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
            } else {
                reason = "scan occupancy excluded every candidate";
                failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
            }
        }
        localization_calculate_set_rejection(record_out, reason, failure_class);
        return;
    }
    record_out->solved = true;
    record_out->source_position = result->map_position;
    record_out->emission_time_us = result->emission_time_us;
    record_out->rms_residual_us = result->rms_residual_us;
    if (velocity_estimated) {
        record_out->estimated_p_velocity_mps = result->velocity_mean_mps;
    }
    record_out->uncertainty.std_m = result->std_m;
    record_out->uncertainty.credible_level = result->credible_level;
    record_out->uncertainty.credible_volume_m3 = result->credible_volume_m3;
    record_out->uncertainty.credible_area_m2 = result->credible_area_m2;
    record_out->uncertainty.credible_min = result->credible_min;
    record_out->uncertainty.credible_max = result->credible_max;
    record_out->uncertainty.sigma_inflation = result->sigma_inflation >= 1.0 ? result->sigma_inflation : 1.0;
    record_out->confidence_score = localization_calculate_quality_from_residual_us(
        result->rms_residual_us, timing_uncertainty_us
    );
}


double localization_calculate_s_minus_p_sigma_us(
    double timing_uncertainty_us,
    double p_snr,
    double s_snr
)
{
    double ratio = 10.0;
    if (s_snr > 0.0 && isfinite(s_snr)) {
        ratio = p_snr > 0.0 && isfinite(p_snr) ? p_snr / s_snr : 1.0;
        ratio = ratio < 1.0 ? 1.0 : (ratio > 10.0 ? 10.0 : ratio);
    }
    const double s_sigma = timing_uncertainty_us * ratio;
    return sqrt(timing_uncertainty_us * timing_uncertainty_us + s_sigma * s_sigma);
}


double localization_calculate_scan_eikonal_velocity(
    const localization_point3d_t *point,
    void *context
)
{
    const localization_calculate_scan_velocity_context_t *scan =
        (const localization_calculate_scan_velocity_context_t *)context;
    if (scan == NULL || point == NULL) {
        return 0.0;
    }
    void *occupancy = (void *)scan->occupancy;
    if (localization_calculate_scan_suea_cell(point, occupancy) != LOCALIZATION_SUEA_CELL_EMPTY) {
        return scan->velocity_mps;
    }
    const double r = scan->sample_radius_m > 0.0 ? scan->sample_radius_m : 0.0;
    const unsigned corners = scan->planar ? 4U : 8U;
    for (unsigned corner = 0U; r > 0.0 && corner < corners; ++corner) {
        const localization_point3d_t sample = {
            point->x_m + ((corner & 1U) != 0U ? r : -r),
            point->y_m + ((corner & 2U) != 0U ? r : -r),
            point->z_m + (scan->planar ? 0.0 : ((corner & 4U) != 0U ? r : -r)),
        };
        if (localization_calculate_scan_suea_cell(&sample, occupancy) != LOCALIZATION_SUEA_CELL_EMPTY) {
            return scan->velocity_mps;
        }
    }
    return 0.0;
}


double localization_calculate_discretization_error_us(double cell_m, double velocity_mps)
{
    if (!(cell_m > 0.0) || !isfinite(cell_m) || !(velocity_mps > 0.0) || !isfinite(velocity_mps)) {
        return 0.0;
    }
    return cell_m / (velocity_mps * sqrt(6.0)) * 1.0e6;
}


void localization_calculate_budget_init(
    localization_calculate_budget_t *budget,
    localization_calculate_clock_fn now_us,
    int64_t deadline_us,
    localization_calculate_progress_fn progress,
    void *progress_context,
    localization_calculate_summary_t *summary,
    double max_horizontal_uncertainty_m
)
{
    memset(budget, 0, sizeof(*budget));
    budget->now_us = now_us;
    budget->deadline_us = deadline_us;
    budget->progress = progress;
    budget->progress_context = progress_context;
    budget->summary = summary;
    budget->max_horizontal_uncertainty_m = max_horizontal_uncertainty_m;
}


bool localization_calculate_budget_begin(
    localization_calculate_budget_t *budget,
    const char *method_name,
    bool skippable
)
{
    budget->method = method_name;
    budget->started_us = budget->now_us != NULL ? budget->now_us() : 0;
    budget->allowed = !skippable || budget->deadline_us <= 0 || budget->started_us < budget->deadline_us;
    if (budget->progress != NULL) {
        budget->progress(
            method_name,
            false,
            budget->summary != NULL ? budget->summary->methods_completed : 0U,
            budget->summary != NULL ? budget->summary->methods_total : 0U,
            budget->progress_context
        );
    }
    return budget->allowed;
}


void localization_calculate_budget_end(
    localization_calculate_budget_t *budget,
    localization_run_record_t *record
)
{
    const int64_t ended = budget->now_us != NULL ? budget->now_us() : budget->started_us;
    localization_calculate_summary_t *summary = budget->summary;
    if (summary != NULL && summary->timing_count < LOCALIZATION_CALCULATE_MAX_TIMINGS) {
        localization_calculate_method_timing_t *timing = &summary->timings[summary->timing_count++];
        snprintf(timing->method_name, sizeof(timing->method_name), "%s",
                 budget->method != NULL ? budget->method : "");
        const int64_t elapsed_ms = (ended - budget->started_us) / 1000;
        timing->duration_ms = elapsed_ms > 0 ? (uint32_t)elapsed_ms : 0U;
        timing->skipped_for_time = !budget->allowed;
    }
    if (!budget->allowed) {
        if (summary != NULL) {
            summary->methods_skipped_for_time++;
        }
        if (record != NULL) {
            record->confidence_score = 0.0;
            localization_calculate_set_rejection(
                record, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON, LOCALIZATION_FAILURE_CLASS_INTERNAL
            );
        }
    } else if (
        record != NULL && record->solved && record->uncertainty.credible_level > 0.0 &&
        hypot(record->uncertainty.std_m.x_m, record->uncertainty.std_m.y_m) >
            budget->max_horizontal_uncertainty_m
    ) {
        record->uncertainty_too_high = true;
        record->failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
    }
    if (summary != NULL && summary->methods_completed < summary->methods_total) {
        ++summary->methods_completed;
    }
    if (budget->progress != NULL) {
        budget->progress(
            budget->method,
            true,
            summary != NULL ? summary->methods_completed : 0U,
            summary != NULL ? summary->methods_total : 0U,
            budget->progress_context
        );
    }
    budget->method = NULL;
}
