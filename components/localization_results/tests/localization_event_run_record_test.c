#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_event_run_record.h"


static localization_event_result_t solved_event_result(void)
{
    localization_event_result_t result;
    memset(&result, 0, sizeof(result));

    result.status = LOCALIZATION_EVENT_SOLVED;
    result.channel_mask_used = 0x0Fu;
    result.channels_used_count = 4u;
    result.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    result.solver_result = (localization_result_t) {
        .source_position = { 1.25, 0.5, 2.0 },
        .emission_time_us = 987654321LL,
        .rms_residual_us = 40.0,
        .iterations = 6U,
        .converged = true
    };

    return result;
}


static void test_solved_result_converts_and_round_trips(void)
{
    const localization_event_result_t event_result = solved_event_result();
    localization_run_record_t record;

    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 123456789ULL, 123400000ULL,
            1U, "bench_calibration", 340.0, 1.25, 50.0, &record
        ) == ESP_OK
    );

    assert(strcmp(record.object_id, "OBJ_0007") == 0);
    assert(strcmp(record.session_id, "S000003") == 0);
    assert(record.kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD);
    assert(strcmp(record.method_name, "tdoa_gauss_newton") == 0);
    assert(record.method_version == 1U);
    assert(record.channel_mask_used == 0x0Fu);
    assert(record.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(record.geometry_computed_at_ms == 123400000ULL);
    assert(strcmp(record.propagation_model_name, "bench_calibration") == 0);
    assert(record.propagation_speed_mps == 340.0);
    assert(record.propagation_speed_uncertainty_mps == 1.25);
    assert(record.solved);
    assert(record.source_position.x_m == 1.25);
    assert(record.emission_time_us == 987654321LL);
    assert(record.rms_residual_us == 40.0);
    assert(record.iterations == 6U);
    assert(record.converged);

    /* residual 40us over a 50us sigma -> exp(-0.5*0.8^2) */
    const double expected_confidence = exp(-0.5 * 0.8 * 0.8);
    assert(fabs(record.confidence_score - expected_confidence) < 1e-9);

    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(fabs(parsed.confidence_score - expected_confidence) < 1e-9);
    free(json);
}


static void test_zero_timing_uncertainty_leaves_confidence_at_zero(void)
{
    const localization_event_result_t event_result = solved_event_result();
    localization_run_record_t record;

    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 0ULL, 0ULL,
            1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_OK
    );

    assert(record.solved);
    assert(record.confidence_score == 0.0);
    assert(record.propagation_model_name[0] == '\0');
}


static void test_insufficient_channels_produces_the_shared_policy_reason(void)
{
    localization_event_result_t event_result;
    memset(&event_result, 0, sizeof(event_result));
    event_result.status = LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS;
    event_result.failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
    event_result.dimensionality = LOCALIZATION_DIMENSIONALITY_INSUFFICIENT;

    localization_run_record_t record;
    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 0ULL, 0ULL,
            1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_OK
    );

    assert(!record.solved);
    assert(record.failure_class == LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN);
    assert(
        strcmp(
            record.rejection_reason,
            "fewer than 3 participating REMOTE geophones: no localization method can run"
        ) == 0
    );

    /* Must be a genuinely valid, persistable record. */
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    free(json);
}


static void test_method_unavailable_for_dimensionality_produces_the_shared_policy_reason(void)
{
    localization_event_result_t event_result;
    memset(&event_result, 0, sizeof(event_result));
    event_result.status = LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY;
    event_result.failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
    event_result.dimensionality = LOCALIZATION_DIMENSIONALITY_PLANAR_2D;

    localization_run_record_t record;
    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 0ULL, 0ULL,
            1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_OK
    );

    assert(!record.solved);
    assert(record.failure_class == LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS);
    assert(
        strcmp(
            record.rejection_reason,
            "exactly 3 participating REMOTE geophones (planar/2D only): "
            "this method requires 4+ for a 3D-capable solve"
        ) == 0
    );
}


static void test_solver_rejected_produces_a_non_empty_reason(void)
{
    localization_event_result_t event_result;
    memset(&event_result, 0, sizeof(event_result));
    event_result.status = LOCALIZATION_EVENT_SOLVER_REJECTED;
    event_result.failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
    event_result.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;

    localization_run_record_t record;
    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 0ULL, 0ULL,
            1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_OK
    );

    assert(!record.solved);
    assert(record.failure_class == LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS);
    assert(record.rejection_reason[0] != '\0');
    assert(strstr(record.rejection_reason, "degenerate") != NULL);
}


static void test_rejects_null_arguments(void)
{
    const localization_event_result_t event_result = solved_event_result();
    localization_run_record_t record;

    assert(
        localization_event_result_to_run_record(
            NULL, "OBJ_0007", "S000003", 0ULL, 0ULL, 1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_0007", "S000003", 0ULL, 0ULL, 1U, "", 340.0, 0.0, 0.0, NULL
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_event_result_to_run_record(
            &event_result, NULL, "S000003", 0ULL, 0ULL, 1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_an_object_id_too_long_for_the_schema(void)
{
    const localization_event_result_t event_result = solved_event_result();
    localization_run_record_t record;

    assert(
        localization_event_result_to_run_record(
            &event_result, "OBJ_THIS_ID_IS_WAY_TOO_LONG_FOR_THE_FIELD", "S000003",
            0ULL, 0ULL, 1U, "", 340.0, 0.0, 0.0, &record
        ) == ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_solved_result_converts_and_round_trips();
    test_zero_timing_uncertainty_leaves_confidence_at_zero();
    test_insufficient_channels_produces_the_shared_policy_reason();
    test_method_unavailable_for_dimensionality_produces_the_shared_policy_reason();
    test_solver_rejected_produces_a_non_empty_reason();
    test_rejects_null_arguments();
    test_rejects_an_object_id_too_long_for_the_schema();
    puts("localization event run record tests passed");
    return 0;
}
