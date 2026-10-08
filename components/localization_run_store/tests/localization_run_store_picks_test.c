#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "localization_run_store.h"
#include "storage_manager.h"


static const char *s_read_failure_path;


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_read_failure_path != NULL && strcmp(path, s_read_failure_path) == 0) {
        return ESP_FAIL;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t read_count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool truncated = fgetc(file) != EOF;
    fclose(file);
    if (truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[read_count] = '\0';
    return ESP_OK;
}


esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok ? ESP_OK : ESP_FAIL;
}


static void ensure_dir(const char *path)
{
    (void)mkdir(path, 0775);
}


static void prepare_session_directory(const char *object_id, const char *session_id)
{
    char path[256];
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)snprintf(path, sizeof(path), "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    (void)snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    (void)snprintf(path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
    ensure_dir(path);
}


static localization_pick_record_t make_pick_record(
    const char *object_id,
    const char *session_id,
    uint64_t computed_at_ms
)
{
    localization_pick_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.object_id, sizeof(record.object_id), "%s", object_id);
    snprintf(record.session_id, sizeof(record.session_id), "%s", session_id);
    record.computed_at_ms = computed_at_ms;

    record.channels[0].participating = true;
    record.channels[0].pick.status = GEOPHONE_PICK_ACCEPTED;
    record.channels[0].pick.arrival_sample_index = 500U;
    record.channels[0].pick.arrival_time_s = 1.0f;
    record.channels[0].pick.snr = 10.0f;
    record.channels[0].pick.uncertainty_samples = 3U;

    for (size_t i = 1U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        record.channels[i].participating = true;
        record.channels[i].pick.status = GEOPHONE_PICK_REJECTED_LOW_SNR;
    }
    return record;
}


static localization_pick_record_t make_max_pick_record(void)
{
    localization_pick_record_t record = make_pick_record(
        "OBJ_0005", "S000005", 9007199254740991ULL
    );
    record.session_epoch_known = true;
    record.session_epoch_timestamp_us = INT64_MIN;
    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        localization_pick_channel_t *channel = &record.channels[i];
        channel->participating = true;
        channel->pick.status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
        channel->pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
        channel->pick.arrival_sample_index = (size_t)9007199254740991ULL;
        channel->pick.arrival_subsample_offset = -0.999f;
        channel->pick.arrival_time_s = FLT_MAX;
        channel->pick.snr = FLT_MAX;
        channel->pick.uncertainty_samples = (size_t)9007199254740991ULL;
        memset(channel->reason, 'r', sizeof(channel->reason) - 1U);
        channel->reason[sizeof(channel->reason) - 1U] = '\0';

        channel->s_pick.present = true;
        channel->s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELDS_ALL;
        channel->s_pick.status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
        channel->s_pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
        channel->s_pick.sample_index = (size_t)9007199254740991ULL;
        channel->s_pick.subsample_offset = -0.999f;
        channel->s_pick.arrival_time_us = DBL_MAX;
        channel->s_pick.snr = DBL_MAX;
        channel->s_pick.uncertainty_us = DBL_MAX;
        memset(channel->s_pick.reason, 's', sizeof(channel->s_pick.reason) - 1U);
        channel->s_pick.reason[sizeof(channel->s_pick.reason) - 1U] = '\0';
    }
    return record;
}


static void cleanup_session_picks(const char *object_id, const char *session_id)
{
    localization_pick_record_t records[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t count = 0U;
    if (
        localization_run_store_list_picks(
            object_id, session_id, records, LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count
        ) != ESP_OK
    ) {
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        char path[LOCALIZATION_RUN_STORE_PATH_MAX];
        if (
            localization_run_store_build_picks_path(
                object_id, session_id, records[i].computed_at_ms, path, sizeof(path)
            ) == ESP_OK
        ) {
            (void)remove(path);
        }
    }
}


static void test_write_and_list_round_trip(void)
{
    prepare_session_directory("OBJ_0003", "S000001");
    cleanup_session_picks("OBJ_0003", "S000001");

    const localization_pick_record_t first = make_pick_record("OBJ_0003", "S000001", 5000ULL);
    const localization_pick_record_t second = make_pick_record("OBJ_0003", "S000001", 6000ULL);
    assert(localization_run_store_write_picks("OBJ_0003", "S000001", &first) == ESP_OK);
    assert(localization_run_store_write_picks("OBJ_0003", "S000001", &second) == ESP_OK);

    localization_pick_record_t records[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t count = 0U;
    assert(
        localization_run_store_list_picks(
            "OBJ_0003", "S000001", records, LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count
        ) == ESP_OK
    );
    /* Different computed_at_ms: both survive as separate files, matching
     * the same "never overwrite" guarantee run records already have. */
    assert(count == 2U);
    for (size_t i = 0U; i < count; ++i) {
        assert(records[i].computed_at_ms == 5000ULL || records[i].computed_at_ms == 6000ULL);
        assert(records[i].channels[0].pick.status == GEOPHONE_PICK_ACCEPTED);
        assert(records[i].channels[0].pick.arrival_sample_index == 500U);
        assert(records[i].channels[1].pick.status == GEOPHONE_PICK_REJECTED_LOW_SNR);
    }

    cleanup_session_picks("OBJ_0003", "S000001");
}


static void test_pick_files_never_collide_with_run_record_files(void)
{
    prepare_session_directory("OBJ_0004", "S000001");
    cleanup_session_picks("OBJ_0004", "S000001");

    const localization_pick_record_t pick = make_pick_record("OBJ_0004", "S000001", 7000ULL);
    assert(localization_run_store_write_picks("OBJ_0004", "S000001", &pick) == ESP_OK);

    /* The run-record listing must not pick up the pick-record file (and
     * vice versa -- verified by test_write_and_list_round_trip above only
     * ever seeing pick records it wrote). */
    localization_run_record_t run_records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t run_count = 0U;
    assert(
        localization_run_store_list(
            "OBJ_0004", "S000001", run_records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &run_count
        ) == ESP_OK
    );
    assert(run_count == 0U);

    cleanup_session_picks("OBJ_0004", "S000001");
}


static void test_missing_session_directory_is_not_found(void)
{
    localization_pick_record_t records[4];
    size_t count = 99U;
    assert(
        localization_run_store_list_picks("OBJ_9999", "S000099", records, 4U, &count) == ESP_ERR_NOT_FOUND
    );

    localization_run_store_list_status_t status;
    count = 99U;
    assert(localization_run_store_list_picks_ex(
        "OBJ_9999", "S000099", records, 4U, &count, &status
    ) == ESP_ERR_NOT_FOUND);
    assert(count == 0U && !status.partial_error && status.partial_error_count == 0U);
}


static void test_partial_error_codes_and_legacy_list_behavior(void)
{
    prepare_session_directory("OBJ_0006", "S000006");
    cleanup_session_picks("OBJ_0006", "S000006");
    localization_pick_record_t valid = make_pick_record("OBJ_0006", "S000006", 6000ULL);
    assert(localization_run_store_write_picks("OBJ_0006", "S000006", &valid) == ESP_OK);

    char corrupt_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(localization_run_store_build_picks_path(
        "OBJ_0006", "S000006", 6001ULL, corrupt_path, sizeof(corrupt_path)
    ) == ESP_OK);
    assert(storage_manager_atomic_write_text(corrupt_path, "not-json") == ESP_OK);

    localization_pick_record_t records[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t count = 0U;
    localization_run_store_list_status_t status;
    assert(localization_run_store_list_picks_ex(
        "OBJ_0006", "S000006", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 1U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED) != 0U);

    count = 0U;
    assert(localization_run_store_list_picks(
        "OBJ_0006", "S000006", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count
    ) == ESP_OK);
    assert(count == 1U);

    assert(remove(corrupt_path) == 0);
    char valid_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(localization_run_store_build_picks_path(
        "OBJ_0006", "S000006", valid.computed_at_ms, valid_path, sizeof(valid_path)
    ) == ESP_OK);
    s_read_failure_path = valid_path;
    count = 0U;
    assert(localization_run_store_list_picks_ex(
        "OBJ_0006", "S000006", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 0U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED) != 0U);
    s_read_failure_path = NULL;
    cleanup_session_picks("OBJ_0006", "S000006");

    char non_directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    (void)snprintf(non_directory_path, sizeof(non_directory_path),
        "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0007", "S000007");
    char parent_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    (void)snprintf(parent_path, sizeof(parent_path),
        "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0007");
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    ensure_dir(parent_path);
    (void)snprintf(parent_path, sizeof(parent_path),
        "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0007");
    ensure_dir(parent_path);
    FILE *not_directory = fopen(non_directory_path, "wb");
    assert(not_directory != NULL);
    assert(fclose(not_directory) == 0);
    count = 0U;
    assert(localization_run_store_list_picks_ex(
        "OBJ_0007", "S000007", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count, &status
    ) == ESP_FAIL);
    assert(count == 0U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED) != 0U);
    count = 99U;
    assert(localization_run_store_list_picks(
        "OBJ_0007", "S000007", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count
    ) == ESP_ERR_NOT_FOUND);
    assert(count == 0U);
    (void)remove(non_directory_path);
}


static void test_max_pick_record_with_s_picks_fits_store_buffer(void)
{
    prepare_session_directory("OBJ_0005", "S000005");
    cleanup_session_picks("OBJ_0005", "S000005");
    localization_pick_record_t original = make_max_pick_record();
    char *json = NULL;
    assert(localization_pick_record_to_json(&original, &json) == ESP_OK);
    const size_t json_length = strlen(json);
    printf("  maximal v1 picks record with four S-picks: %u bytes, store buffer %u bytes\n",
        (unsigned)json_length, (unsigned)LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    fflush(stdout);
    assert(json_length + 1U <= LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    free(json);

    const localization_pick_record_t store_record = original;
    assert(localization_run_store_write_picks("OBJ_0005", "S000005", &store_record) == ESP_OK);
    localization_pick_record_t records[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t count = 0U;
    localization_run_store_list_status_t status;
    assert(localization_run_store_list_picks_ex(
        "OBJ_0005", "S000005", records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 1U && !status.partial_error);
    assert(records[0].session_epoch_known);
    assert(records[0].session_epoch_timestamp_us == store_record.session_epoch_timestamp_us);
    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        assert(records[0].channels[i].s_pick.present);
        assert(records[0].channels[i].s_pick.sample_index == original.channels[i].s_pick.sample_index);
        assert(records[0].channels[i].s_pick.arrival_time_us == original.channels[i].s_pick.arrival_time_us);
    }
    cleanup_session_picks("OBJ_0005", "S000005");
}


static void test_rejects_invalid_arguments(void)
{
    localization_pick_record_t records[4];
    size_t count = 0U;
    assert(localization_run_store_list_picks("OBJ_0003", "S000001", NULL, 4U, &count) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list_picks("OBJ_0003", "S000001", records, 0U, &count) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list_picks("OBJ_0003", "S000001", records, 4U, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list_picks("not-an-object", "S000001", records, 4U, &count) == ESP_ERR_INVALID_ARG);

    assert(localization_run_store_write_picks("OBJ_0003", "S000001", NULL) == ESP_ERR_INVALID_ARG);

    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(
        localization_run_store_build_picks_path("not-an-object", "S000001", 1ULL, path, sizeof(path))
        == ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_max_pick_record_with_s_picks_fits_store_buffer();
    test_write_and_list_round_trip();
    test_pick_files_never_collide_with_run_record_files();
    test_partial_error_codes_and_legacy_list_behavior();
    test_missing_session_directory_is_not_found();
    test_rejects_invalid_arguments();
    puts("localization run store picks tests passed");
    return 0;
}
