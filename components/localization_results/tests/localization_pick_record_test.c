#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_pick_record.h"


static localization_pick_record_t sample_record(void)
{
    localization_pick_record_t record;
    memset(&record, 0, sizeof(record));

    record.schema_version = LOCALIZATION_PICK_RECORD_SCHEMA_VERSION;
    snprintf(record.object_id, sizeof(record.object_id), "OBJ_0007");
    snprintf(record.session_id, sizeof(record.session_id), "S000003");
    record.computed_at_ms = 123456789ULL;

    record.channels[0].participating = true;
    record.channels[0].pick.status = GEOPHONE_PICK_ACCEPTED;
    record.channels[0].pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
    record.channels[0].pick.arrival_sample_index = 1200U;
    record.channels[0].pick.arrival_time_s = 2.4f;
    record.channels[0].pick.snr = 14.5f;
    record.channels[0].pick.uncertainty_samples = 5U;

    record.channels[1].participating = true;
    record.channels[1].pick.status = GEOPHONE_PICK_REJECTED_LOW_SNR;
    record.channels[1].pick.arrival_sample_index = 900U;
    record.channels[1].pick.snr = 1.2f;

    record.channels[2].participating = false;
    record.channels[2].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;

    record.channels[3].participating = true;
    record.channels[3].pick.status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
    record.channels[3].pick.arrival_sample_index = 1000U;

    return record;
}


static void assert_records_equal(
    const localization_pick_record_t *expected,
    const localization_pick_record_t *actual
)
{
    assert(expected->schema_version == actual->schema_version);
    assert(strcmp(expected->object_id, actual->object_id) == 0);
    assert(strcmp(expected->session_id, actual->session_id) == 0);
    assert(expected->computed_at_ms == actual->computed_at_ms);
    assert(expected->session_epoch_known == actual->session_epoch_known);
    assert(expected->session_epoch_timestamp_us == actual->session_epoch_timestamp_us);

    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const localization_pick_channel_t *e = &expected->channels[i];
        const localization_pick_channel_t *a = &actual->channels[i];
        assert(e->participating == a->participating);
        assert(e->pick.status == a->pick.status);
        assert(e->pick.reason == a->pick.reason);
        assert(e->pick.method == a->pick.method);
        assert(e->pick.arrival_sample_index == a->pick.arrival_sample_index);
        assert(fabsf(e->pick.arrival_subsample_offset - a->pick.arrival_subsample_offset) < 1e-6f);
        assert(fabsf(e->pick.arrival_time_s - a->pick.arrival_time_s) < 1e-6f);
        assert(fabsf(e->pick.snr - a->pick.snr) < 1e-6f);
        assert(e->pick.uncertainty_samples == a->pick.uncertainty_samples);
        assert(strcmp(e->reason, a->reason) == 0);
        assert(e->s_pick.present == a->s_pick.present);
        if (e->s_pick.present) {
            assert(e->s_pick.fields_present == a->s_pick.fields_present);
            assert(e->s_pick.status == a->s_pick.status);
            assert(strcmp(e->s_pick.reason, a->s_pick.reason) == 0);
            assert(e->s_pick.method == a->s_pick.method);
            assert(e->s_pick.sample_index == a->s_pick.sample_index);
            assert(fabsf(e->s_pick.subsample_offset - a->s_pick.subsample_offset) < 1e-6f);
            assert(fabs(e->s_pick.arrival_time_us - a->s_pick.arrival_time_us) < 1e-9);
            assert(fabs(e->s_pick.snr - a->s_pick.snr) < 1e-9);
            assert(fabs(e->s_pick.uncertainty_us - a->s_pick.uncertainty_us) < 1e-9);
        }
    }
}


static void test_round_trip(void)
{
    localization_pick_record_t record = sample_record();
    record.schema_version = LOCALIZATION_PICK_RECORD_SCHEMA_VERSION;

    char *json = NULL;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    assert(json != NULL);
    assert(strstr(json, "\"accepted\"") != NULL);
    assert(strstr(json, "\"rejected_low_snr\"") != NULL);
    assert(strstr(json, "\"sta_lta_refined\"") != NULL);

    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    free(json);

    assert_records_equal(&record, &parsed);
}


static void test_wave_a_statuses_reasons_and_s_picks_round_trip(void)
{
    localization_pick_record_t original = sample_record();
    original.session_epoch_known = true;
    original.session_epoch_timestamp_us = 1700000000123456LL;

    original.channels[0].pick.status = GEOPHONE_PICK_REJECTED_SATURATED;
    original.channels[0].pick.reason = GEOPHONE_PICK_REASON_SATURATED;
    snprintf(original.channels[0].reason, sizeof(original.channels[0].reason), "saturated");
    original.channels[0].s_pick.present = true;
    original.channels[0].s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELDS_ALL;
    original.channels[0].s_pick.status = GEOPHONE_PICK_ACCEPTED;
    original.channels[0].s_pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
    original.channels[0].s_pick.sample_index = 1250U;
    original.channels[0].s_pick.subsample_offset = -0.25f;
    original.channels[0].s_pick.arrival_time_us = 2500123.5;
    original.channels[0].s_pick.snr = 12.75;
    original.channels[0].s_pick.uncertainty_us = 40.0;

    original.channels[1].pick.status = GEOPHONE_PICK_REJECTED_AMBIGUOUS;
    original.channels[1].pick.reason = GEOPHONE_PICK_REASON_AMBIGUOUS;
    snprintf(original.channels[1].reason, sizeof(original.channels[1].reason), "ambiguous");
    original.channels[1].s_pick.present = true;
    original.channels[1].s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELD_STATUS;
    original.channels[1].s_pick.status = GEOPHONE_PICK_REJECTED_AMBIGUOUS;
    snprintf(original.channels[1].s_pick.reason, sizeof(original.channels[1].s_pick.reason), "ambiguous");

    original.channels[2].s_pick.present = true;
    original.channels[2].s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELD_STATUS;
    original.channels[2].s_pick.status = GEOPHONE_PICK_REJECTED_LOW_SNR;

    char *json = NULL;
    assert(localization_pick_record_to_json(&original, &json) == ESP_OK);
    assert(strstr(json, "\"status\":\"rejected_saturated\"") != NULL);
    assert(strstr(json, "\"status\":\"rejected_ambiguous\"") != NULL);
    assert(strstr(json, "\"reason\":\"saturated\"") != NULL);
    assert(strstr(json, "\"s_pick\":{") != NULL);
    assert(strstr(json, "\"arrival_time_us\":2500123.5") != NULL);
    assert(strstr(json, "\"status\":\"rejected_ambiguous\",\"reason\":\"ambiguous\"") != NULL);
    assert(strstr(json, "\"status\":\"rejected_low_snr\"") != NULL);

    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    assert_records_equal(&original, &parsed);
    free(json);
}


static void test_old_v1_record_defaults_optional_fields(void)
{
    const char *old_json =
        "{\"schema_version\":1,\"object_id\":\"OBJ_0007\",\"session_id\":\"S000003\","
        "\"computed_at_ms\":123,\"channels\":["
        "{\"participating\":true,\"status\":\"accepted\",\"method\":\"sta_lta_refined\",\"arrival_sample_index\":10,\"arrival_time_s\":0.02,\"snr\":5,\"uncertainty_samples\":2},"
        "{\"participating\":false,\"status\":\"rejected_no_trigger\",\"method\":\"sta_lta_refined\",\"arrival_sample_index\":0,\"arrival_time_s\":0,\"snr\":0,\"uncertainty_samples\":0},"
        "{\"participating\":false,\"status\":\"rejected_no_trigger\",\"method\":\"sta_lta_refined\",\"arrival_sample_index\":0,\"arrival_time_s\":0,\"snr\":0,\"uncertainty_samples\":0},"
        "{\"participating\":false,\"status\":\"rejected_no_trigger\",\"method\":\"sta_lta_refined\",\"arrival_sample_index\":0,\"arrival_time_s\":0,\"snr\":0,\"uncertainty_samples\":0}]}";
    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(old_json, &parsed) == ESP_OK);
    assert(parsed.schema_version == LOCALIZATION_PICK_RECORD_SCHEMA_VERSION);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        assert(parsed.channels[i].reason[0] == '\0');
        assert(!parsed.channels[i].s_pick.present);
    }
    assert(parsed.channels[0].pick.arrival_subsample_offset == 0.0f);
}


static void test_epoch_zero_is_distinct_from_unknown(void)
{
    localization_pick_record_t record = sample_record();
    char *json = NULL;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_known\":false") != NULL);
    assert(strstr(json, "\"session_epoch_timestamp_us\":null") != NULL);
    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    assert(!parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    free(json);

    record.session_epoch_known = true;
    record.session_epoch_timestamp_us = 0;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_known\":true") != NULL);
    assert(strstr(json, "\"session_epoch_timestamp_us\":0") != NULL);
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.session_epoch_known && parsed.session_epoch_timestamp_us == 0);
    free(json);

    record.session_epoch_timestamp_us = INT64_MAX;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"session_epoch_timestamp_us\":9223372036854775807") != NULL);
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.session_epoch_known && parsed.session_epoch_timestamp_us == INT64_MAX);
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
    memcpy(result + prefix_length + new_length, position + old_length,
        json_length - prefix_length - old_length + 1U);
    return result;
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

static void assert_pick_number_rejected(const char *key, const char *literal)
{
    localization_pick_record_t record = sample_record();
    char *json = NULL;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    char *invalid = with_number(json, key, literal);
    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(invalid, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(invalid);
    free(json);
}

/*
 * Numbers read from a stored record are data. A fractional, negative,
 * non-finite or oversized value must be rejected before it is converted to an
 * integer type (the bare cast is undefined behaviour), not truncated.
 */
static void test_integer_fields_reject_fractional_negative_and_oversized_numbers(void)
{
    const char *const bad[] = { "1e30", "1.5", "-1", "1e999", "18446744073709551616" };
    const char *const keys[] = {
        "computed_at_ms", "arrival_sample_index", "uncertainty_samples", "schema_version",
    };
    for (size_t key = 0U; key < sizeof(keys) / sizeof(keys[0]); ++key) {
        for (size_t value = 0U; value < sizeof(bad) / sizeof(bad[0]); ++value) {
            assert_pick_number_rejected(keys[key], bad[value]);
        }
    }

    /* Control: the unmodified record still parses. */
    localization_pick_record_t record = sample_record();
    char *json = NULL;
    localization_pick_record_t parsed;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    assert(localization_pick_record_from_json(json, &parsed) == ESP_OK);
    assert(parsed.channels[0].pick.arrival_sample_index == 1200U);
    free(json);
}


static void test_bad_status_strings_and_values_are_rejected(void)
{
    localization_pick_record_t record = sample_record();
    char *json = NULL;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    char *invalid = replace_json_once(
        json, "\"status\":\"accepted\"", "\"status\":\"rejected_future\""
    );
    localization_pick_record_t parsed;
    assert(localization_pick_record_from_json(invalid, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(invalid);
    free(json);

    record = sample_record();
    record.channels[0].s_pick.present = true;
    record.channels[0].s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELD_STATUS;
    record.channels[0].s_pick.status = GEOPHONE_PICK_ACCEPTED;
    assert(localization_pick_record_to_json(&record, &json) == ESP_OK);
    invalid = replace_json_once(
        json, "\"s_pick\":{\"status\":\"accepted\"}",
        "\"s_pick\":{\"status\":\"rejected_future\"}"
    );
    assert(localization_pick_record_from_json(invalid, &parsed) == ESP_ERR_INVALID_RESPONSE);
    free(invalid);
    free(json);

    record.channels[0].s_pick.status = (geophone_pick_status_t)99;
    assert(localization_pick_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);

    record = sample_record();
    record.channels[0].pick.status = (geophone_pick_status_t)99;
    assert(localization_pick_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_rejects_null_arguments(void)
{
    localization_pick_record_t record = sample_record();
    char *json = NULL;
    assert(localization_pick_record_to_json(NULL, &json) == ESP_ERR_INVALID_ARG);
    assert(localization_pick_record_to_json(&record, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_pick_record_from_json(NULL, &record) == ESP_ERR_INVALID_ARG);
    assert(localization_pick_record_from_json("{}", NULL) == ESP_ERR_INVALID_ARG);
}


static void test_rejects_non_finite_accepted_pick(void)
{
    localization_pick_record_t record = sample_record();
    record.channels[0].pick.status = GEOPHONE_PICK_ACCEPTED;
    record.channels[0].pick.arrival_time_s = NAN;

    char *json = NULL;
    assert(localization_pick_record_to_json(&record, &json) == ESP_ERR_INVALID_ARG);
}


static void test_rejects_malformed_json(void)
{
    localization_pick_record_t record;
    assert(localization_pick_record_from_json("not-json-at-all", &record) == ESP_ERR_INVALID_RESPONSE);
}


static void test_rejects_unknown_schema_version(void)
{
    localization_pick_record_t record;
    assert(
        localization_pick_record_from_json(
            "{\"schema_version\":99,\"object_id\":\"OBJ_0007\",\"session_id\":\"S000003\","
            "\"computed_at_ms\":1,\"channels\":[]}",
            &record
        ) == ESP_ERR_NOT_FOUND
    );
}


static void test_rejects_missing_channel_entries(void)
{
    localization_pick_record_t record;
    /* Only 2 of the required 4 channel entries present. */
    assert(
        localization_pick_record_from_json(
            "{\"schema_version\":1,\"object_id\":\"OBJ_0007\",\"session_id\":\"S000003\","
            "\"computed_at_ms\":1,\"channels\":["
            "{\"channel\":1,\"participating\":true,\"status\":\"accepted\",\"method\":\"sta_lta_refined\","
            "\"arrival_sample_index\":1,\"arrival_time_s\":0.1,\"snr\":5.0,\"uncertainty_samples\":1},"
            "{\"channel\":2,\"participating\":true,\"status\":\"accepted\",\"method\":\"sta_lta_refined\","
            "\"arrival_sample_index\":1,\"arrival_time_s\":0.1,\"snr\":5.0,\"uncertainty_samples\":1}"
            "]}",
            &record
        ) == ESP_ERR_INVALID_RESPONSE
    );
}


int main(void)
{
    test_integer_fields_reject_fractional_negative_and_oversized_numbers();
    test_round_trip();
    test_wave_a_statuses_reasons_and_s_picks_round_trip();
    test_old_v1_record_defaults_optional_fields();
    test_epoch_zero_is_distinct_from_unknown();
    test_bad_status_strings_and_values_are_rejected();
    test_rejects_null_arguments();
    test_rejects_non_finite_accepted_pick();
    test_rejects_malformed_json();
    test_rejects_unknown_schema_version();
    test_rejects_missing_channel_entries();
    puts("localization pick record tests passed");
    return 0;
}
