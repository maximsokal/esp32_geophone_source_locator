#include "localization_event_run_record.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


static esp_err_t copy_bounded(
    const char *text,
    char *out,
    size_t out_size
)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(out, out_size, "%s", text);
    return
        written < 0 || written >= (int)out_size
            ? ESP_ERR_INVALID_ARG
            : ESP_OK;
}


static const char *rejection_reason_for_status(
    localization_event_status_t status
)
{
    switch (status) {
        case LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS:
            return localization_method_availability_reason(
                LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS
            );

        case LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY:
            return localization_method_availability_reason(
                LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D
            );

        case LOCALIZATION_EVENT_SOLVER_REJECTED:
            return
                "TDOA solver rejected the input (sensor layout too flat to "
                "resolve depth, degenerate geometry or non-finite estimate)";

        case LOCALIZATION_EVENT_SOLVED:
        default:
            return "";
    }
}


esp_err_t localization_event_result_to_run_record(
    const localization_event_result_t *event_result,
    const char *object_id,
    const char *session_id,
    uint64_t computed_at_ms,
    uint64_t geometry_computed_at_ms,
    uint32_t method_version,
    const char *propagation_model_name,
    const char *material,
    double propagation_speed_mps,
    double propagation_speed_uncertainty_mps,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
)
{
    if (event_result == NULL || record_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    /* Redundancy is attached by the caller that knows the search dimensionality. */
    record.degrees_of_freedom = -1;

    esp_err_t err = copy_bounded(object_id, record.object_id, sizeof(record.object_id));
    if (err != ESP_OK) {
        return err;
    }
    err = copy_bounded(session_id, record.session_id, sizeof(record.session_id));
    if (err != ESP_OK) {
        return err;
    }
    err = copy_bounded(
        propagation_model_name == NULL ? "" : propagation_model_name,
        record.propagation_model_name,
        sizeof(record.propagation_model_name)
    );
    if (err != ESP_OK) {
        return err;
    }
    err = copy_bounded(material == NULL ? "" : material, record.material, sizeof(record.material));
    if (err != ESP_OK) {
        return err;
    }

    record.computed_at_ms = computed_at_ms;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    err = copy_bounded("tdoa_gauss_newton", record.method_name, sizeof(record.method_name));
    if (err != ESP_OK) {
        return err;
    }
    record.method_version = method_version;
    record.channel_mask_used = event_result->channel_mask_used;
    record.channel_mask_rejected = event_result->channel_mask_rejected;
    record.dimensionality = event_result->dimensionality;
    record.geometry_computed_at_ms = geometry_computed_at_ms;
    record.propagation_speed_mps = propagation_speed_mps;
    record.propagation_speed_uncertainty_mps = propagation_speed_uncertainty_mps;

    record.solved = event_result->status == LOCALIZATION_EVENT_SOLVED;
    record.failure_class = event_result->failure_class;

    if (record.solved) {
        const localization_result_t *solver_result = &event_result->solver_result;

        record.source_position = solver_result->source_position;
        record.emission_time_us = solver_result->emission_time_us;
        record.rms_residual_us = solver_result->rms_residual_us;
        record.iterations = solver_result->iterations;
        record.converged = solver_result->converged;

        if (timing_uncertainty_us > 0.0) {
            const double normalized = solver_result->rms_residual_us / timing_uncertainty_us;
            record.confidence_score = exp(-0.5 * normalized * normalized);
        }
    } else {
        err = copy_bounded(
            rejection_reason_for_status(event_result->status),
            record.rejection_reason,
            sizeof(record.rejection_reason)
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    *record_out = record;
    return ESP_OK;
}
