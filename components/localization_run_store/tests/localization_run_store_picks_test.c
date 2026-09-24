#include <assert.h>
#include <stdio.h>
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


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
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
    test_write_and_list_round_trip();
    test_pick_files_never_collide_with_run_record_files();
    test_missing_session_directory_is_not_found();
    test_rejects_invalid_arguments();
    puts("localization run store picks tests passed");
    return 0;
}
