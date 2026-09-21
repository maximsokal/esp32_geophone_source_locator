#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "localization_event_bridge.h"
#include "localization_run_record.h"

/*
 * MAX-135/MAX-140: converts a localization_event_bridge result (the wired
 * Gauss-Newton TDOA pipeline) into a localization_run_record_t ready for
 * localization_run_store_write() (a separate component -- this file does
 * not touch the filesystem). Kept as its own small translation layer
 * rather than folded into either side, so localization_event_bridge stays
 * unaware of the persistence schema and localization_run_record stays
 * unaware of any specific method.
 *
 * confidence_score is computed only when timing_uncertainty_us > 0 (the
 * caller's own measured/assumed pick-timing noise, e.g. from MAX-142's
 * jitter finding): exp(-0.5 * (rms_residual_us / timing_uncertainty_us)^2),
 * the same Gaussian quality convention localization_comparison's header
 * documents for this exact method family (MAX-138). timing_uncertainty_us
 * <= 0 leaves confidence_score at 0 ("not computed"), never a guessed
 * value.
 */

esp_err_t localization_event_result_to_run_record(
    const localization_event_result_t *event_result,
    const char *object_id,
    const char *session_id,
    uint64_t computed_at_ms,
    uint64_t geometry_computed_at_ms,
    uint32_t method_version,
    const char *propagation_model_name,
    double propagation_speed_mps,
    double propagation_speed_uncertainty_mps,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);
