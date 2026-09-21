#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_run_record.h"


static localization_run_record_t solved_record(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));

    snprintf(record.object_id, sizeof(record.object_id), "OBJ_0007");
    snprintf(record.session_id, sizeof(record.session_id), "S000003");
    record.computed_at_ms = 123456789ULL;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(record.method_name, sizeof(record.method_name), "tdoa_gauss_newton");
    record.method_version = 1U;
    record.channel_mask_used = 0x0Fu;
    record.channel_mask_rejected = 0u;
    record.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    record.geometry_computed_at_ms = 123400000ULL;
    snprintf(
        record.propagation_model_name,
        sizeof(record.propagation_model_name),
        "bench_calibration"
    );
    record.propagation_speed_mps = 340.0;
    record.propagation_speed_uncertainty_mps = 1.25;
    record.solved = true;
    record.source_position = (localization_point3d_t) { 1.25, 0.5, 2.0 };
    record.emission_time_us = 987654321LL;
    record.rms_residual_us = 0.42;
    record.iterations = 6U;
    record.converged = true;

    return record;
}


static localization_run_record_t rejected_record(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));

    snprintf(record.object_id, sizeof(record.object_id), "OBJ_0007");
    snprintf(record.session_id, sizeof(record.session_id), "S000003");
    record.computed_at_ms = 123456789ULL;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(record.method_name, sizeof(record.method_name), "tdoa_gauss_newton");
    record.method_version = 1U;
    record.channel_mask_used = 0x0Du;
    record.channel_mask_rejected = 0x02u;
    record.dimensionality = LOCALIZATION_DIMENSIONALITY_PLANAR_2D;
    record.geometry_computed_at_ms = 123400000ULL;
    record.propagation_speed_mps = 340.0;
    record.solved = false;
    snprintf(
        record.rejection_reason,
        sizeof(record.rejection_reason),
        "exactly 3 participating REMOTE geophones (planar/2D only): "
        "this method requires 4+ for a 3D-capable solve"
    );

    return record;
}


static void test_solved_record_round_trips(void)
{
    const localization_run_record_t original = solved_record();
    char *json = NULL;

    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(json != NULL);
    assert(strstr(json, "\"kind\":\"individual_method\"") != NULL);
    assert(strstr(json, "\"dimensionality\":\"capable_3d\"") != NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);

    assert(strcmp(parsed.object_id, original.object_id) == 0);
    assert(strcmp(parsed.session_id, original.session_id) == 0);
    assert(parsed.computed_at_ms == original.computed_at_ms);
    assert(parsed.kind == original.kind);
    assert(strcmp(parsed.method_name, original.method_name) == 0);
    assert(parsed.method_version == original.method_version);
    assert(parsed.channel_mask_used == original.channel_mask_used);
    assert(parsed.channel_mask_rejected == original.channel_mask_rejected);
    assert(parsed.dimensionality == original.dimensionality);
    assert(parsed.geometry_computed_at_ms == original.geometry_computed_at_ms);
    assert(strcmp(parsed.propagation_model_name, original.propagation_model_name) == 0);
    assert(fabs(parsed.propagation_speed_mps - original.propagation_speed_mps) < 1e-9);
    assert(
        fabs(
            parsed.propagation_speed_uncertainty_mps -
            original.propagation_speed_uncertainty_mps
        ) < 1e-9
    );
    assert(parsed.solved);
    assert(fabs(parsed.source_position.x_m - original.source_position.x_m) < 1e-9);
    assert(fabs(parsed.source_position.y_m - original.source_position.y_m) < 1e-9);
    assert(fabs(parsed.source_position.z_m - original.source_position.z_m) < 1e-9);
    assert(parsed.emission_time_us == original.emission_time_us);
    assert(fabs(parsed.rms_residual_us - original.rms_residual_us) < 1e-9);
    assert(parsed.iterations == original.iterations);
    assert(parsed.converged == original.converged);

    free(json);
}


static void test_rejected_record_round_trips_with_reason(void)
{
    const localization_run_record_t original = rejected_record();
    char *json = NULL;

    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(strstr(json, "\"solved\":false") != NULL);
    assert(strstr(json, "rejection_reason") != NULL);
    assert(strstr(json, "\"source_position\"") == NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(!parsed.solved);
    assert(strcmp(parsed.rejection_reason, original.rejection_reason) == 0);
    /* An unsolved record must never carry a stale/zeroed XYZ that looks
     * like a real answer. */
    assert(parsed.source_position.x_m == 0.0);
    assert(parsed.emission_time_us == 0);

    free(json);
}


static void test_unsolved_record_without_reason_is_rejected(void)
{
    localization_run_record_t record = rejected_record();
    record.rejection_reason[0] = '\0';
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_solved_record_with_non_finite_position_is_rejected(void)
{
    localization_run_record_t record = solved_record();
    record.source_position.x_m = NAN;
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_negative_propagation_uncertainty_is_rejected(void)
{
    localization_run_record_t record = solved_record();
    record.propagation_speed_uncertainty_mps = -1.0;
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_unknown_propagation_uncertainty_round_trips_as_zero(void)
{
    /* "0 means unknown", not "exactly zero uncertainty" -- a caller that
     * never calibrated a model must still be able to persist a run. */
    localization_run_record_t record = solved_record();
    record.propagation_model_name[0] = '\0';
    record.propagation_speed_uncertainty_mps = 0.0;
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_OK);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.propagation_model_name[0] == '\0');
    assert(parsed.propagation_speed_uncertainty_mps == 0.0);

    free(json);
}


static void test_confidence_score_round_trips(void)
{
    /* MAX-136/137/138: the field that lets a semblance/traveltime-
     * inversion/joint-fusion run persist its own native quality score,
     * since it has no "iterations"/"converged" of its own. */
    localization_run_record_t record = solved_record();
    record.confidence_score = 0.9263;
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"confidence_score\":0.9263") != NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(fabs(parsed.confidence_score - 0.9263) < 1e-9);

    free(json);
}


static void test_negative_confidence_score_is_rejected(void)
{
    localization_run_record_t record = solved_record();
    record.confidence_score = -0.1;
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_future_schema_version_is_not_found(void)
{
    const char *future_json =
        "{\"schema_version\":999,\"object_id\":\"OBJ_0001\","
        "\"session_id\":\"S000001\",\"computed_at_ms\":0,"
        "\"kind\":\"individual_method\",\"method_name\":\"future_method\","
        "\"method_version\":1,\"channel_mask_used\":0,"
        "\"channel_mask_rejected\":0,\"dimensionality\":\"capable_3d\","
        "\"geometry_computed_at_ms\":0,\"propagation_speed_mps\":340.0,"
        "\"solved\":false,\"rejection_reason\":\"n/a\"}";

    localization_run_record_t parsed;
    assert(
        localization_run_record_from_json(future_json, &parsed) ==
        ESP_ERR_NOT_FOUND
    );
}


static void test_malformed_json_is_invalid_response(void)
{
    localization_run_record_t parsed;
    assert(
        localization_run_record_from_json("not json", &parsed) ==
        ESP_ERR_INVALID_RESPONSE
    );

    /* Current schema version, but missing every other required field --
     * must be INVALID_RESPONSE (malformed), not NOT_FOUND (wrong version). */
    char current_version_only[64];
    snprintf(
        current_version_only,
        sizeof(current_version_only),
        "{\"schema_version\":%u}",
        (unsigned)LOCALIZATION_RUN_SCHEMA_VERSION
    );
    assert(
        localization_run_record_from_json(current_version_only, &parsed) ==
        ESP_ERR_INVALID_RESPONSE
    );
}


static void test_rejects_null_arguments(void)
{
    localization_run_record_t record = solved_record();
    char *json = NULL;
    localization_run_record_t parsed;

    assert(localization_run_record_to_json(NULL, &json) == ESP_ERR_INVALID_ARG);
    assert(localization_run_record_to_json(&record, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_run_record_from_json(NULL, &parsed) == ESP_ERR_INVALID_ARG);
    assert(localization_run_record_from_json("{}", NULL) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_solved_record_round_trips();
    test_rejected_record_round_trips_with_reason();
    test_unsolved_record_without_reason_is_rejected();
    test_solved_record_with_non_finite_position_is_rejected();
    test_negative_propagation_uncertainty_is_rejected();
    test_unknown_propagation_uncertainty_round_trips_as_zero();
    test_confidence_score_round_trips();
    test_negative_confidence_score_is_rejected();
    test_future_schema_version_is_not_found();
    test_malformed_json_is_invalid_response();
    test_rejects_null_arguments();
    puts("localization run record json tests passed");
    return 0;
}
