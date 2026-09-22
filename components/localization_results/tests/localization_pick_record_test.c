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

    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const localization_pick_channel_t *e = &expected->channels[i];
        const localization_pick_channel_t *a = &actual->channels[i];
        assert(e->participating == a->participating);
        assert(e->pick.status == a->pick.status);
        assert(e->pick.method == a->pick.method);
        assert(e->pick.arrival_sample_index == a->pick.arrival_sample_index);
        assert(fabsf(e->pick.arrival_time_s - a->pick.arrival_time_s) < 1e-6f);
        assert(fabsf(e->pick.snr - a->pick.snr) < 1e-6f);
        assert(e->pick.uncertainty_samples == a->pick.uncertainty_samples);
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
    test_round_trip();
    test_rejects_null_arguments();
    test_rejects_non_finite_accepted_pick();
    test_rejects_malformed_json();
    test_rejects_unknown_schema_version();
    test_rejects_missing_channel_entries();
    puts("localization pick record tests passed");
    return 0;
}
