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
    record.schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    record.scan_provenance_present = true;

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
    record.schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    record.scan_provenance_present = true;

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
    record.failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
    snprintf(
        record.rejection_reason,
        sizeof(record.rejection_reason),
        "exactly 3 participating REMOTE geophones (planar/2D only): "
        "this method requires 4+ for a 3D-capable solve"
    );

    return record;
}



/* Replaces the numeric token that follows "key": with `literal`. */
static char *with_number(const char *json, const char *key, const char *literal)
{
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(json, needle);
    assert(at != NULL);
    const char *value = at + strlen(needle);
    const char *end = value;
    while (*end != '\0' && *end != ',' && *end != '}' && *end != ']') {
        ++end;
    }
    const size_t prefix = (size_t)(value - json);
    const size_t literal_length = strlen(literal);
    char *result = malloc(prefix + literal_length + strlen(end) + 1U);
    assert(result != NULL);
    memcpy(result, json, prefix);
    memcpy(result + prefix, literal, literal_length);
    memcpy(result + prefix + literal_length, end, strlen(end) + 1U);
    return result;
}

static void assert_run_number_rejected(const char *key, const char *literal)
{
    localization_run_record_t record = solved_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    char *invalid = with_number(json, key, literal);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(invalid, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(invalid);
    free(json);
}

static void test_integer_fields_reject_fractional_negative_and_oversized_numbers(void)
{
    const char *const bad_any[] = { "1e30", "1.5", "-1", "1e999", "18446744073709551616" };
    const char *const bad_32bit[] = { "4294967296" };
    const char *const keys[] = {
        "computed_at_ms", "geometry_computed_at_ms", "method_version",
        "channel_mask_used", "channel_mask_rejected", "iterations",
    };
    for (size_t key = 0U; key < sizeof(keys) / sizeof(keys[0]); ++key) {
        for (size_t value = 0U; value < sizeof(bad_any) / sizeof(bad_any[0]); ++value) {
            assert_run_number_rejected(keys[key], bad_any[value]);
        }
    }
    assert_run_number_rejected("method_version", bad_32bit[0]);
    assert_run_number_rejected("channel_mask_used", bad_32bit[0]);
    assert_run_number_rejected("channel_mask_rejected", bad_32bit[0]);

    /* Does not fit the uint8_t iteration counter. */
    assert_run_number_rejected("iterations", "256");

    /* int64 emission time: fractional and out-of-range are rejected. */
    assert_run_number_rejected("emission_time_us", "1e30");
    assert_run_number_rejected("emission_time_us", "-1e30");
    assert_run_number_rejected("emission_time_us", "2.5");
    assert_run_number_rejected("emission_time_us", "9223372036854775808");

    /* Control: boundary values that do fit are still accepted. */
    localization_run_record_t record = solved_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    char *valid = with_number(json, "iterations", "255");
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(valid, &parsed) == ESP_OK);
    assert(parsed.iterations == 255U);
    free(valid);
    valid = with_number(json, "channel_mask_used", "4294967295");
    assert(localization_run_record_from_json(valid, &parsed) == ESP_OK);
    assert(parsed.channel_mask_used == UINT32_MAX);
    free(valid);
    free(json);
}


static void test_solved_record_round_trips(void)
{
    const localization_run_record_t original = solved_record();
    char *json = NULL;

    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(json != NULL);
    assert(strstr(json, "\"kind\":\"individual_method\"") != NULL);
    assert(strstr(json, "\"dimensionality\":\"capable_3d\"") != NULL);
    assert(strstr(json, "\"failure_class\":\"none\"") != NULL);

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
    assert(parsed.terminal_status == LOCALIZATION_RUN_TERMINAL_STATUS_NONE);
    assert(!parsed.session_epoch_known);
    assert(parsed.session_epoch_timestamp_us == 0);

    free(json);
}


static void test_rejected_record_round_trips_with_reason(void)
{
    const localization_run_record_t original = rejected_record();
    char *json = NULL;

    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(strstr(json, "\"solved\":false") != NULL);
    assert(strstr(json, "\"failure_class\":\"record_again\"") != NULL);
    assert(strstr(json, "rejection_reason") != NULL);
    assert(strstr(json, "\"source_position\"") == NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(!parsed.solved);
    assert(parsed.failure_class == LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN);
    assert(strcmp(parsed.rejection_reason, original.rejection_reason) == 0);
    /* An unsolved record must never carry a stale/zeroed XYZ that looks
     * like a real answer. */
    assert(parsed.source_position.x_m == 0.0);
    assert(parsed.emission_time_us == 0);

    free(json);
}


static void test_old_rejected_record_without_failure_class_reads_as_none(void)
{
    localization_run_record_t old = rejected_record();
    old.schema_version = LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY;
    old.failure_class = LOCALIZATION_FAILURE_CLASS_NONE;
    char *json = NULL;
    assert(localization_run_record_to_json(&old, &json) == ESP_OK);
    assert(strstr(json, "\"schema_version\":3") != NULL);

    char *field = strstr(json, ",\"failure_class\":\"none\"");
    assert(field != NULL);
    memmove(field, field + strlen(",\"failure_class\":\"none\""),
            strlen(field + strlen(",\"failure_class\":\"none\"")) + 1U);
    field = strstr(json, ",\"uncertainty_too_high\":false");
    assert(field != NULL);
    memmove(field, field + strlen(",\"uncertainty_too_high\":false"),
            strlen(field + strlen(",\"uncertainty_too_high\":false")) + 1U);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY);
    assert(!parsed.solved && parsed.failure_class == LOCALIZATION_FAILURE_CLASS_NONE);
    assert(!parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.terminal_status == LOCALIZATION_RUN_TERMINAL_STATUS_NONE);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    assert(strstr(json, "scan_constraint_applied") == NULL);
    free(json);
}


static void test_uncertainty_flag_round_trips_and_requires_geometry_class(void)
{
    localization_run_record_t record = solved_record();
    record.uncertainty_too_high = true;
    record.failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
    record.uncertainty.credible_level = 0.9;
    record.uncertainty.std_m = (localization_point3d_t) { 1.5, 1.5, 0.2 };
    record.uncertainty.credible_min = (localization_point3d_t) { 1.0, 0.0, 1.0 };
    record.uncertainty.credible_max = (localization_point3d_t) { 2.0, 1.0, 3.0 };
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"uncertainty_too_high\":true") != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.uncertainty_too_high);
    assert(parsed.failure_class == LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS);
    free(json);

    record.failure_class = LOCALIZATION_FAILURE_CLASS_NONE;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_unsolved_record_without_reason_is_rejected(void)
{
    localization_run_record_t record = rejected_record();
    record.rejection_reason[0] = '\0';
    char *json = NULL;

    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_unsolved_record_with_reason_but_no_class_is_rejected(void)
{
    localization_run_record_t record = rejected_record();
    record.failure_class = LOCALIZATION_FAILURE_CLASS_NONE;
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);

    record.failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    char *field = strstr(json, "\"failure_class\":\"record_again\"");
    assert(field != NULL);
    memcpy(field + strlen("\"failure_class\":\""), "none", strlen("none"));
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(json);
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



/* MAX-212: redundancy round-trips; a record written before the field
 * existed reads back as -1 ("unknown"), never as 0 ("exactly determined"). */
static void test_degrees_of_freedom_round_trips_and_defaults(void)
{
    localization_run_record_t record = solved_record();
    record.degrees_of_freedom = 0;
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"degrees_of_freedom\":0") != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.degrees_of_freedom == 0);

    char *legacy = strstr(json, ",\"degrees_of_freedom\":0");
    assert(legacy != NULL);
    memmove(legacy, legacy + strlen(",\"degrees_of_freedom\":0"),
            strlen(legacy + strlen(",\"degrees_of_freedom\":0")) + 1U);
    assert(strstr(json, "degrees_of_freedom") == NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.degrees_of_freedom == -1);
    free(json);
}


static void test_material_round_trips_and_is_optional_for_old_records(void)
{
    localization_run_record_t record = solved_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"material\"") == NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.material[0] == '\0');
    free(json);

    snprintf(record.material, sizeof(record.material), "concrete");
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"material\":\"concrete\"") != NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(strcmp(parsed.material, "concrete") == 0);
    free(json);
}


static void test_truncated_analysis_window_is_provenance(void)
{
    localization_run_record_t record = solved_record();
    record.analysis_window_truncated = true;
    record.analysis_window_start_index = 0U;
    record.analysis_window_frames = 104857U;
    snprintf(record.analysis_window_reason, sizeof(record.analysis_window_reason), "memory");

    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(
        json,
        "\"analysis_window\":{\"start_index\":0,\"frames\":104857,\"truncated\":true,\"reason\":\"memory\"}"
    ) != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.analysis_window_truncated);
    assert(parsed.analysis_window_start_index == 0U);
    assert(parsed.analysis_window_frames == 104857U);
    assert(strcmp(parsed.analysis_window_reason, "memory") == 0);
    free(json);

    record.analysis_window_frames = 0U;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);

    record = solved_record();
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"analysis_window\"") == NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(!parsed.analysis_window_truncated);
    free(json);
}


static void test_estimated_velocities_round_trip_and_stay_optional(void)
{
    /* A velocity-free method reports the velocities it estimated. */
    localization_run_record_t record = solved_record();
    snprintf(record.method_name, sizeof(record.method_name), "psafuvs");
    record.estimated_p_velocity_mps = 321.5;
    record.estimated_s_velocity_mps = 171.25;
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"estimated_p_velocity_mps\":321.5") != NULL);
    assert(strstr(json, "\"estimated_s_velocity_mps\":171.25") != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.estimated_p_velocity_mps == 321.5);
    assert(parsed.estimated_s_velocity_mps == 171.25);
    free(json);

    /* Every other method omits them; old records parse as "not estimated". */
    record = solved_record();
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "estimated_") == NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.estimated_p_velocity_mps == 0.0 && parsed.estimated_s_velocity_mps == 0.0);
    free(json);

    /* An unsolved record never carries them. */
    record = rejected_record();
    record.estimated_p_velocity_mps = 300.0;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "estimated_") == NULL);
    free(json);

    record = solved_record();
    record.estimated_s_velocity_mps = NAN;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
    record.estimated_s_velocity_mps = -1.0;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_uncertainty_round_trips_and_stays_optional(void)
{
    /* A Bayesian run reports its posterior region. */
    localization_run_record_t record = solved_record();
    snprintf(record.method_name, sizeof(record.method_name), "bayesian");
    record.uncertainty.std_m = (localization_point3d_t) { 0.25, 0.5, 1.5 };
    record.uncertainty.credible_level = 0.9;
    record.uncertainty.credible_volume_m3 = 0.75;
    record.uncertainty.credible_min = (localization_point3d_t) { 1.0, 2.0, -3.0 };
    record.uncertainty.credible_max = (localization_point3d_t) { 1.5, 2.5, -0.5 };
    record.uncertainty.sigma_inflation = 2.5;
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strlen(json) < 2048U); /* LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE */
    assert(strstr(json, "\"uncertainty\":{\"std_m\":{\"x_m\":0.25,\"y_m\":0.5,\"z_m\":1.5}") != NULL);
    assert(strstr(json, "\"credible_level\":0.9") != NULL);
    assert(strstr(json, "\"sigma_inflation\":2.5") != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(memcmp(&parsed.uncertainty, &record.uncertainty, sizeof(record.uncertainty)) == 0);
    free(json);

    /* Every other method omits it; old records parse as "not computed". */
    record = solved_record();
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"uncertainty\"") == NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.uncertainty.credible_level == 0.0);
    free(json);

    /* An unsolved record never carries it. */
    record = rejected_record();
    record.uncertainty.credible_level = 0.9;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"uncertainty\"") == NULL);
    free(json);

    /* Inconsistent regions are refused on write and on read. */
    record = solved_record();
    record.uncertainty.credible_level = 1.0;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
    record.uncertainty.credible_level = 0.9;
    record.uncertainty.credible_min.x_m = 2.0; /* min > max */
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
    record.uncertainty.credible_min.x_m = 0.0;
    record.uncertainty.std_m.z_m = -1.0;
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
    record.uncertainty.std_m.z_m = 1.0;
    record.uncertainty.sigma_inflation = 0.5; /* a region is never narrowed */
    assert(localization_run_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);

    /* A record written before sigma_inflation existed reads it as 1. */
    {
        const char *old = "{\"schema_version\":3,\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
            "\"computed_at_ms\":1,\"kind\":\"individual_method\",\"method_name\":\"bayesian\",\"method_version\":1,"
            "\"channel_mask_used\":15,\"channel_mask_rejected\":0,\"dimensionality\":\"capable_3d\","
            "\"geometry_computed_at_ms\":1,\"propagation_model_name\":\"\",\"propagation_speed_mps\":300,"
            "\"propagation_speed_uncertainty_mps\":0,\"confidence_score\":0.5,\"solved\":true,"
            "\"source_position\":{\"x_m\":1,\"y_m\":2,\"z_m\":-1},\"emission_time_us\":0,\"rms_residual_us\":1,"
            "\"iterations\":0,\"converged\":false,\"uncertainty\":{\"std_m\":{\"x_m\":0.1,\"y_m\":0.1,\"z_m\":0.1},"
            "\"credible_level\":0.9,\"credible_volume_m3\":0.01,\"credible_area_m2\":0,"
            "\"credible_min\":{\"x_m\":0.9,\"y_m\":1.9,\"z_m\":-1.1},\"credible_max\":{\"x_m\":1.1,\"y_m\":2.1,\"z_m\":-0.9}}}";
        localization_run_record_t old_record;
        assert(localization_run_record_from_json(old, &old_record) == ESP_OK);
        assert(old_record.uncertainty.sigma_inflation == 1.0);
    }

    record = solved_record();
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    char broken[2048];
    const char *tail = strrchr(json, '}');
    const size_t head = (size_t)(tail - json);
    snprintf(broken, sizeof(broken), "%.*s,\"uncertainty\":{\"credible_level\":0.9}}", (int)head, json);
    assert(localization_run_record_from_json(broken, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(json);
}


/* MAX-301 S4 and MAX-132 wave B: range/status and scan provenance round-trip in v7. */
static void test_velocity_range_and_status_round_trip_in_v7(void)
{
    localization_run_record_t original = solved_record();
    original.velocity_range_min_mps = 3000.0f;
    original.velocity_range_max_mps = 4800.0f;
    original.velocity_range_vp_vs_min = 1.55f;
    original.velocity_range_vp_vs_max = 1.8f;
    original.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_PRESET;
    original.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_AT_BOUND;

    char *json = NULL;
    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(strstr(json, "\"schema_version\":7") != NULL);
    assert(strstr(json, "\"velocity_range\"") != NULL);
    assert(strstr(json, "\"source\":\"preset\"") != NULL);
    assert(strstr(json, "\"velocity_status\":\"at_bound\"") != NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION);
    assert(parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.velocity_range_min_mps == 3000.0f);
    assert(parsed.velocity_range_max_mps == 4800.0f);
    assert(fabsf(parsed.velocity_range_vp_vs_min - 1.55f) < 1e-6f);
    assert(fabsf(parsed.velocity_range_vp_vs_max - 1.8f) < 1e-6f);
    assert(parsed.velocity_range_source == LOCALIZATION_RUN_VELOCITY_SOURCE_PRESET);
    assert(parsed.velocity_status == LOCALIZATION_RUN_VELOCITY_STATUS_AT_BOUND);
    free(json);

    /* No range and UNKNOWN status: nothing extra is written. */
    localization_run_record_t plain = solved_record();
    assert(localization_run_record_to_json(&plain, &json) == ESP_OK);
    assert(strstr(json, "velocity_range") == NULL);
    assert(strstr(json, "velocity_status") == NULL);
    free(json);
}


static void test_scan_provenance_round_trips_in_v7(void)
{
    localization_run_record_t original = solved_record();
    original.scan_constraint_applied = true;
    snprintf(original.scan_frame_id, sizeof(original.scan_frame_id), "scan-crc32:deadbeef");
    original.occupancy_created_at_ms = 234567U;
    original.registration_geometry_computed_at_ms = 123456U;

    char *json = NULL;
    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    assert(strstr(json, "\"scan_constraint_applied\":true") != NULL);
    assert(strstr(json, "\"scan_frame_id\":\"scan-crc32:deadbeef\"") != NULL);
    assert(strstr(json, "\"occupancy_created_at_ms\":234567") != NULL);
    assert(strstr(json, "\"registration_geometry_computed_at_ms\":123456") != NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION);
    assert(parsed.scan_provenance_present && parsed.scan_constraint_applied);
    assert(strcmp(parsed.scan_frame_id, original.scan_frame_id) == 0);
    assert(parsed.occupancy_created_at_ms == original.occupancy_created_at_ms);
    assert(parsed.registration_geometry_computed_at_ms ==
        original.registration_geometry_computed_at_ms);
    free(json);

    localization_run_record_t unconstrained = solved_record();
    assert(localization_run_record_to_json(&unconstrained, &json) == ESP_OK);
    assert(strstr(json, "\"scan_constraint_applied\":false") != NULL);
    assert(strstr(json, "\"scan_frame_id\":null") != NULL);
    assert(strstr(json, "\"occupancy_created_at_ms\":0") != NULL);
    assert(strstr(json, "\"registration_geometry_computed_at_ms\":0") != NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.scan_frame_id[0] == '\0');
    assert(parsed.occupancy_created_at_ms == 0U);
    assert(parsed.registration_geometry_computed_at_ms == 0U);
    free(json);
}


static char *replace_json_once(const char *json, const char *old_text, const char *new_text)
{
    const char *position = strstr(json, old_text);
    assert(position != NULL);
    const size_t prefix_length = (size_t)(position - json);
    const size_t old_length = strlen(old_text);
    const size_t new_length = strlen(new_text);
    const size_t json_length = strlen(json);
    char *result = malloc(json_length - old_length + new_length + 1U);
    assert(result != NULL);
    memcpy(result, json, prefix_length);
    memcpy(result + prefix_length, new_text, new_length);
    memcpy(
        result + prefix_length + new_length,
        position + old_length,
        json_length - prefix_length - old_length + 1U
    );
    return result;
}


static void test_v6_record_reads_with_v7_defaults(void)
{
    const char *v6 =
        "{\"schema_version\":6,\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"computed_at_ms\":1,\"kind\":\"individual_method\",\"method_name\":\"tdoa\","
        "\"method_version\":1,\"channel_mask_used\":15,\"channel_mask_rejected\":0,"
        "\"dimensionality\":\"capable_3d\",\"geometry_computed_at_ms\":1,"
        "\"propagation_model_name\":\"\",\"propagation_speed_mps\":400,"
        "\"propagation_speed_uncertainty_mps\":0,\"confidence_score\":0.5,"
        "\"solved\":true,\"failure_class\":\"none\",\"uncertainty_too_high\":false,"
        "\"scan_constraint_applied\":false,\"scan_frame_id\":null,"
        "\"occupancy_created_at_ms\":0,\"registration_geometry_computed_at_ms\":0,"
        "\"source_position\":{\"x_m\":1,\"y_m\":2,\"z_m\":3},"
        "\"emission_time_us\":0,\"rms_residual_us\":1,\"iterations\":0,\"converged\":false}";
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(v6, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION_V6);
    assert(parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.terminal_status == LOCALIZATION_RUN_TERMINAL_STATUS_NONE);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
}


static void test_terminal_status_and_epoch_round_trip(void)
{
    static const char *const names[] = {
        "none", "iteration_limit", "time_budget_skipped", "time_budget_hit",
        "cell_budget", "resolution_limited"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        localization_run_record_t original = solved_record();
        original.terminal_status = (localization_run_terminal_status_t)i;
        original.session_epoch_known = true;
        original.session_epoch_timestamp_us = 1700000000123456LL;
        char *json = NULL;
        assert(localization_run_record_to_json(&original, &json) == ESP_OK);
        assert(strstr(json, "\"terminal_status\":\"") != NULL);
        assert(strstr(json, names[i]) != NULL);

        localization_run_record_t parsed;
        assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
        assert(parsed.solved);
        assert(parsed.terminal_status == original.terminal_status);
        assert(parsed.session_epoch_known);
        assert(parsed.session_epoch_timestamp_us == original.session_epoch_timestamp_us);
        free(json);
    }

    localization_run_record_t unknown = solved_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&unknown, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_known\":false") != NULL);
    assert(strstr(json, "\"session_epoch_timestamp_us\":null") != NULL);
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    free(json);

    localization_run_record_t epoch_zero = solved_record();
    epoch_zero.session_epoch_known = true;
    epoch_zero.session_epoch_timestamp_us = 0;
    assert(localization_run_record_to_json(&epoch_zero, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_known\":true") != NULL);
    assert(strstr(json, "\"session_epoch_timestamp_us\":0") != NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    free(json);

    epoch_zero.session_epoch_timestamp_us = INT64_MAX;
    assert(localization_run_record_to_json(&epoch_zero, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_timestamp_us\":9223372036854775807") != NULL);
    assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.session_epoch_known && parsed.session_epoch_timestamp_us == INT64_MAX);
    free(json);
}


static void test_input_rejection_reason_codes_round_trip(void)
{
    static const char *const reasons[] = {
        LOCALIZATION_RUN_REJECTION_REASON_TIMESTAMP_NON_MONOTONIC,
        LOCALIZATION_RUN_REJECTION_REASON_SESSION_GAP,
        LOCALIZATION_RUN_REJECTION_REASON_DUPLICATE_SENSOR_POSITION,
        LOCALIZATION_RUN_REJECTION_REASON_REFLECTED_TRANSFORM
    };
    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        localization_run_record_t original = rejected_record();
        snprintf(original.rejection_reason, sizeof(original.rejection_reason), "%s", reasons[i]);
        char *json = NULL;
        assert(localization_run_record_to_json(&original, &json) == ESP_OK);
        localization_run_record_t parsed;
        assert(localization_run_record_from_json(json, &parsed) == ESP_OK);
        assert(strcmp(parsed.rejection_reason, reasons[i]) == 0);
        free(json);
    }
}


static void test_unknown_terminal_status_is_rejected(void)
{
    localization_run_record_t original = solved_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    char *invalid = replace_json_once(
        json, "\"terminal_status\":\"none\"", "\"terminal_status\":\"unknown\""
    );
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(invalid, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(invalid);
    free(json);

    original.terminal_status = (localization_run_terminal_status_t)99;
    assert(localization_run_record_to_json(&original, &json) == ESP_ERR_INVALID_ARG);
}


static void test_malformed_v7_scan_provenance_is_rejected(void)
{
    char *json = NULL;
    localization_run_record_t record = solved_record();
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);

    char *broken = replace_json_once(json, ",\"scan_constraint_applied\":false", "");
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(broken, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(broken);

    broken = replace_json_once(
        json, ",\"scan_frame_id\":null", ",\"scan_frame_id\":\"scan-crc32:deadbeef\""
    );
    assert(localization_run_record_from_json(broken, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(broken);

    broken = replace_json_once(
        json, ",\"occupancy_created_at_ms\":0", ",\"occupancy_created_at_ms\":-1"
    );
    assert(localization_run_record_from_json(broken, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(broken);
    free(json);
}


/* MAX-301 S4 (compatibility: passes on old and new code): a v4 record reads,
 * and its range/status stay UNKNOWN. */
static void test_v4_record_reads_with_unknown_velocity_range(void)
{
    const char *v4 =
        "{\"schema_version\":4,\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"computed_at_ms\":1,\"kind\":\"individual_method\",\"method_name\":\"suea\","
        "\"method_version\":1,\"channel_mask_used\":15,\"channel_mask_rejected\":0,"
        "\"dimensionality\":\"capable_3d\",\"geometry_computed_at_ms\":1,"
        "\"propagation_model_name\":\"\",\"propagation_speed_mps\":400,"
        "\"propagation_speed_uncertainty_mps\":250,\"confidence_score\":0.5,"
        "\"solved\":true,\"failure_class\":\"none\",\"uncertainty_too_high\":false,"
        "\"source_position\":{\"x_m\":1,\"y_m\":2,\"z_m\":-1},\"emission_time_us\":0,"
        "\"rms_residual_us\":10,\"iterations\":0,\"converged\":false}";
    localization_run_record_t parsed;
    assert(localization_run_record_from_json(v4, &parsed) == ESP_OK);
    assert(parsed.schema_version == 4U);
    assert(!parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.velocity_range_min_mps == 0.0f && parsed.velocity_range_max_mps == 0.0f);
    assert(parsed.velocity_range_source == LOCALIZATION_RUN_VELOCITY_SOURCE_UNKNOWN);
    assert(parsed.velocity_status == LOCALIZATION_RUN_VELOCITY_STATUS_UNKNOWN);
    assert(parsed.terminal_status == LOCALIZATION_RUN_TERMINAL_STATUS_NONE);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
}


static void test_v5_record_reads_without_scan_provenance(void)
{
    const char *v4 =
        "{\"schema_version\":4,\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"computed_at_ms\":1,\"kind\":\"individual_method\",\"method_name\":\"suea\","
        "\"method_version\":1,\"channel_mask_used\":15,\"channel_mask_rejected\":0,"
        "\"dimensionality\":\"capable_3d\",\"geometry_computed_at_ms\":1,"
        "\"propagation_model_name\":\"\",\"propagation_speed_mps\":400,"
        "\"propagation_speed_uncertainty_mps\":250,\"confidence_score\":0.5,"
        "\"solved\":true,\"failure_class\":\"none\",\"uncertainty_too_high\":false,"
        "\"source_position\":{\"x_m\":1,\"y_m\":2,\"z_m\":-1},\"emission_time_us\":0,"
        "\"rms_residual_us\":10,\"iterations\":0,\"converged\":false}";
    char v5[1024];
    assert(strlen(v4) + 1U <= sizeof(v5));
    strcpy(v5, v4);
    char *version = strstr(v5, "\"schema_version\":4");
    assert(version != NULL);
    version[strlen("\"schema_version\":")] = '5';

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(v5, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION_V5);
    assert(!parsed.scan_provenance_present && !parsed.scan_constraint_applied);
    assert(parsed.scan_frame_id[0] == '\0');
    assert(parsed.occupancy_created_at_ms == 0U);
    assert(parsed.registration_geometry_computed_at_ms == 0U);
    assert(parsed.terminal_status == LOCALIZATION_RUN_TERMINAL_STATUS_NONE);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
}


/* MAX-301 S4 (new behaviour): malformed range/status is rejected on both
 * sides, never silently read as a narrower or unknown range. */
static void test_malformed_velocity_range_is_rejected(void)
{
    localization_run_record_t bad = solved_record();
    bad.velocity_range_min_mps = 4800.0f;
    bad.velocity_range_max_mps = 3000.0f;          /* inverted */
    bad.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_USER;
    char *json = NULL;
    assert(localization_run_record_to_json(&bad, &json) == ESP_ERR_INVALID_ARG);

    bad = solved_record();
    bad.velocity_status = 9U;                      /* not a known status */
    assert(localization_run_record_to_json(&bad, &json) == ESP_ERR_INVALID_ARG);

    bad = solved_record();
    bad.velocity_range_min_mps = 100.0f;
    bad.velocity_range_max_mps = 200.0f;
    bad.velocity_range_vp_vs_min = 1.5f;           /* only one Vp/Vs end */
    bad.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_USER;
    assert(localization_run_record_to_json(&bad, &json) == ESP_ERR_INVALID_ARG);

    localization_run_record_t good = solved_record();
    good.velocity_range_min_mps = 100.0f;
    good.velocity_range_max_mps = 200.0f;
    good.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_USER;
    assert(localization_run_record_to_json(&good, &json) == ESP_OK);
    localization_run_record_t parsed;

    char *tampered = malloc(strlen(json) + 16U);
    assert(tampered != NULL);
    const char *at = strstr(json, "\"user\"");
    assert(at != NULL);
    const size_t head = (size_t)(at - json);
    memcpy(tampered, json, head);
    strcpy(tampered + head, "\"guess\"");
    strcat(tampered, at + strlen("\"user\""));
    assert(localization_run_record_from_json(tampered, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(tampered);
    free(json);
}


/* MAX-301 decision C (new behaviour): an inconsistent run is never primary. */
static void test_inconsistent_run_is_not_a_primary_candidate(void)
{
    localization_run_record_t record = solved_record();
    assert(localization_run_record_is_primary_candidate(&record));          /* UNKNOWN (v3/v4) */
    record.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_OK;
    assert(localization_run_record_is_primary_candidate(&record));
    record.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_AT_BOUND;
    assert(localization_run_record_is_primary_candidate(&record));          /* warned, still usable */
    record.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_INCONSISTENT;
    assert(!localization_run_record_is_primary_candidate(&record));
    localization_run_record_t rejected = rejected_record();
    assert(!localization_run_record_is_primary_candidate(&rejected));
    assert(!localization_run_record_is_primary_candidate(NULL));
}


int main(void)
{
    test_integer_fields_reject_fractional_negative_and_oversized_numbers();
    test_inconsistent_run_is_not_a_primary_candidate();
    test_velocity_range_and_status_round_trip_in_v7();
    test_scan_provenance_round_trips_in_v7();
    test_v6_record_reads_with_v7_defaults();
    test_terminal_status_and_epoch_round_trip();
    test_input_rejection_reason_codes_round_trip();
    test_unknown_terminal_status_is_rejected();
    test_malformed_v7_scan_provenance_is_rejected();
    test_v4_record_reads_with_unknown_velocity_range();
    test_v5_record_reads_without_scan_provenance();
    test_malformed_velocity_range_is_rejected();
    test_solved_record_round_trips();
    test_rejected_record_round_trips_with_reason();
    test_old_rejected_record_without_failure_class_reads_as_none();
    test_uncertainty_flag_round_trips_and_requires_geometry_class();
    test_unsolved_record_without_reason_is_rejected();
    test_unsolved_record_with_reason_but_no_class_is_rejected();
    test_solved_record_with_non_finite_position_is_rejected();
    test_negative_propagation_uncertainty_is_rejected();
    test_unknown_propagation_uncertainty_round_trips_as_zero();
    test_confidence_score_round_trips();
    test_degrees_of_freedom_round_trips_and_defaults();
    test_material_round_trips_and_is_optional_for_old_records();
    test_truncated_analysis_window_is_provenance();
    test_estimated_velocities_round_trip_and_stay_optional();
    test_uncertainty_round_trips_and_stays_optional();
    test_negative_confidence_score_is_rejected();
    test_future_schema_version_is_not_found();
    test_malformed_json_is_invalid_response();
    test_rejects_null_arguments();
    puts("localization run record json tests passed");
    return 0;
}
