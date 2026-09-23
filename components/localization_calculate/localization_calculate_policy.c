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

    if (result == NULL || result->status != LOCALIZATION_TRAVELTIME_SOLVED) {
        record_out->solved = false;
        snprintf(
            record_out->rejection_reason, sizeof(record_out->rejection_reason), "%s",
            "insufficient accepted picks for traveltime inversion"
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
        record_out->solved = false;
        snprintf(
            record_out->rejection_reason, sizeof(record_out->rejection_reason), "%s",
            (not_attempted_reason != NULL && not_attempted_reason[0] != '\0')
                ? not_attempted_reason
                : "semblance search was not attempted"
        );
        return;
    }

    if (!result->found) {
        record_out->solved = false;
        snprintf(
            record_out->rejection_reason, sizeof(record_out->rejection_reason), "%s",
            "semblance search found no coherent candidate"
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

    if (result == NULL || !result->found) {
        record_out->solved = false;
        snprintf(
            record_out->rejection_reason, sizeof(record_out->rejection_reason), "%s",
            "no channel had positive weighted energy"
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
        record_out->solved = false;
        snprintf(
            record_out->rejection_reason, sizeof(record_out->rejection_reason), "%s",
            "no individual method solved with usable quality"
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
        /* MAX-212: TDOA and traveltime inversion fit the same picks --
         * correlated, not independent, evidence. */
        if (
            strcmp(record->method_name, "tdoa_gauss_newton") == 0 ||
            strcmp(record->method_name, "traveltime_inversion") == 0
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
