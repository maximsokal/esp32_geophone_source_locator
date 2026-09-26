#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <unistd.h>
#endif

#define STORAGE_MANAGER_MOUNT_POINT "seismic_recorder_recovery_tmp"

#include "object_store.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "seismic_recorder_internal.h"
#include "storage_manager.h"

static seismic_recorder_snapshot_t fake_live_snapshot;
static bool fake_terminal_present;
static seismic_recorder_session_status_t fake_terminal_status;
static unsigned fake_metadata_write_calls;
static seismic_recorder_snapshot_t fake_last_metadata_snapshot;
static seismic_recorder_session_participation_t fake_last_participation;
static char fake_last_metadata_status[32];
static unsigned fake_storage_begin_calls;
static unsigned fake_storage_end_calls;

typedef enum
{
    RAW_HEADER_VALID = 0,
    RAW_HEADER_BAD_MAGIC,
    RAW_HEADER_BAD_VERSION,
    RAW_HEADER_BAD_CHANNEL_COUNT,
    RAW_HEADER_BAD_FRAME_SIZE,

} raw_header_variant_t;

static int make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static void remove_file_if_present(const char *path)
{
    if (remove(path) != 0) {
        assert(errno == ENOENT);
    }
}

static void reset_layout(void)
{
    const char *session_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001";
    char path[256];

    (void)snprintf(path, sizeof(path), "%s/session.json", session_path);
    remove_file_if_present(path);
    (void)snprintf(path, sizeof(path), "%s/session.json.bak", session_path);
    remove_file_if_present(path);
    (void)snprintf(path, sizeof(path), "%s/session.json.tmp", session_path);
    remove_file_if_present(path);
    (void)snprintf(path, sizeof(path), "%s/samples.bin", session_path);
    remove_file_if_present(path);

    (void)rmdir(session_path);
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}

static void make_layout(void)
{
    reset_layout();
    assert(make_directory(STORAGE_MANAGER_MOUNT_POINT) == 0);
    assert(make_directory(STORAGE_MANAGER_ROOT_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);
}

static void reset_fakes(void)
{
    memset(&fake_live_snapshot, 0, sizeof(fake_live_snapshot));
    fake_terminal_present = false;
    fake_terminal_status = SEISMIC_RECORDER_SESSION_UNKNOWN;
    fake_metadata_write_calls = 0U;
    memset(&fake_last_metadata_snapshot, 0, sizeof(fake_last_metadata_snapshot));
    memset(&fake_last_participation, 0, sizeof(fake_last_participation));
    fake_last_metadata_status[0] = '\0';
    fake_storage_begin_calls = 0U;
    fake_storage_end_calls = 0U;
}

esp_err_t storage_manager_operation_begin(void)
{
    ++fake_storage_begin_calls;
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
    ++fake_storage_end_calls;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    if (object_id == NULL || profile == NULL || strcmp(object_id, "OBJ_0001") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    snprintf(profile->summary.name, sizeof(profile->summary.name), "Recovery test");
    return ESP_OK;
}

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = fake_live_snapshot;
    return ESP_OK;
}

esp_err_t seismic_recorder_fs_build_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    if (object_id == NULL || session_id == NULL || path == NULL || path_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        path,
        path_size,
        "%s/%s/sessions/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id
    );
    return written < 0 || written >= (int)path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static void fill_fake_terminal_summary(
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    memset(summary, 0, sizeof(*summary));
    snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
    snprintf(summary->source_name, sizeof(summary->source_name), "AD7606");
    summary->status = fake_terminal_status;
    summary->complete = fake_terminal_status == SEISMIC_RECORDER_SESSION_COMPLETE;
    summary->readable = fake_terminal_status != SEISMIC_RECORDER_SESSION_UNKNOWN;
    summary->recovered = fake_terminal_status == SEISMIC_RECORDER_SESSION_RECOVERED_ERROR;
    summary->sample_rate_hz = 500U;
    summary->channel_count = SEISMIC_CHANNEL_COUNT;
    summary->samples_written = fake_last_metadata_snapshot.samples_written;
    summary->dropped_frames_available = !summary->recovered;
    summary->recording_ms_available = !summary->recovered;
}

esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        summary == NULL ||
        strcmp(object_id, "OBJ_0001") != 0 ||
        strcmp(session_id, "S000001") != 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!fake_terminal_present) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    fill_fake_terminal_summary(session_id, summary);
    return ESP_OK;
}

esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary(object_id, session_id, summary);
}

esp_err_t seismic_recorder_json_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status,
    const seismic_recorder_session_participation_t *participation
)
{
    assert(profile != NULL);
    assert(snapshot != NULL);
    assert(status != NULL);
    assert(participation != NULL);
    ++fake_metadata_write_calls;
    fake_last_metadata_snapshot = *snapshot;
    fake_last_participation = *participation;
    snprintf(fake_last_metadata_status, sizeof(fake_last_metadata_status), "%s", status);
    fake_terminal_present = true;
    fake_terminal_status = strcmp(status, "recovered_error") == 0
        ? SEISMIC_RECORDER_SESSION_RECOVERED_ERROR
        : strcmp(status, "complete") == 0
            ? SEISMIC_RECORDER_SESSION_COMPLETE
            : SEISMIC_RECORDER_SESSION_ERROR;
    return ESP_OK;
}

static bool fail_recovery_candidate_malloc;

static void *recovery_test_malloc(size_t size)
{
    if (fail_recovery_candidate_malloc) {
        fail_recovery_candidate_malloc = false;
        return NULL;
    }
    return malloc(size);
}


#define malloc recovery_test_malloc
#include "../seismic_recorder_recovery.c"
#undef malloc


static size_t read_file_bytes(const char *path, unsigned char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    const size_t count = fread(buffer, 1U, capacity, file);
    assert(ferror(file) == 0);
    assert(fgetc(file) == EOF);
    assert(ferror(file) == 0);
    assert(fclose(file) == 0);
    return count;
}

static void write_candidate(const char *file_name, const char *text)
{
    char path[256];
    assert(
        snprintf(
            path,
            sizeof(path),
            "%s/OBJ_0001/sessions/S000001/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            file_name
        ) < (int)sizeof(path)
    );
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static const char *valid_recording_metadata(void)
{
    return
        "{\"schema_version\":1,"
        "\"object_id\":\"OBJ_0001\","
        "\"session_id\":\"S000001\","
        "\"status\":\"recording\","
        "\"source\":\"AD7606\","
        "\"sample_rate_hz\":500,"
        "\"channel_count\":4}";
}


esp_err_t seismic_recorder_json_parse_participation_text(
    const char *json,
    seismic_recorder_session_participation_t *participation
)
{
    if (json == NULL || participation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(participation, 0, sizeof(*participation));
    if (strstr(json, "\"schema_version\":2") != NULL) {
        participation->pods[0] = (seismic_recorder_pod_participation_t) {
            .assigned = true,
            .node_id_known = true,
            .node_id = 73U,
            .hardware_id_length = 2U,
            .hardware_id = {0xABU, 0xCDU},
            .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING,
        };
    }
    return ESP_OK;
}


static const char *valid_recording_metadata_v2(void)
{
    return
        "{\"schema_version\":2,"
        "\"object_id\":\"OBJ_0001\","
        "\"session_id\":\"S000001\","
        "\"status\":\"recording\","
        "\"source\":\"AD7606\","
        "\"sample_rate_hz\":500,"
        "\"channel_count\":4}";
}


static const char *unsupported_recording_metadata_v3(void)
{
    return
        "{\"schema_version\":3,"
        "\"object_id\":\"OBJ_0001\","
        "\"session_id\":\"S000001\","
        "\"status\":\"recording\","
        "\"source\":\"AD7606\","
        "\"sample_rate_hz\":500,"
        "\"channel_count\":4}";
}

static uint64_t samples_file_size(void)
{
    char path[256];
    assert(
        snprintf(
            path,
            sizeof(path),
            "%s/OBJ_0001/sessions/S000001/samples.bin",
            STORAGE_MANAGER_OBJECTS_PATH
        ) < (int)sizeof(path)
    );
    struct stat info;
    assert(stat(path, &info) == 0);
    return (uint64_t)info.st_size;
}

static void write_raw(
    size_t full_frames,
    size_t trailing_bytes,
    raw_header_variant_t variant
)
{
    char path[256];
    assert(
        snprintf(
            path,
            sizeof(path),
            "%s/OBJ_0001/sessions/S000001/samples.bin",
            STORAGE_MANAGER_OBJECTS_PATH
        ) < (int)sizeof(path)
    );

    FILE *file = fopen(path, "wb");
    assert(file != NULL);

    seismic_recorder_binary_header_t header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = sizeof(seismic_frame_t),
        .reserved = 0U,
    };

    if (variant == RAW_HEADER_BAD_MAGIC) header.magic[0] = 'X';
    if (variant == RAW_HEADER_BAD_VERSION) ++header.version;
    if (variant == RAW_HEADER_BAD_CHANNEL_COUNT) --header.channel_count;
    if (variant == RAW_HEADER_BAD_FRAME_SIZE) ++header.frame_size_bytes;

    assert(fwrite(&header, sizeof(header), 1U, file) == 1U);

    for (size_t index = 0U; index < full_frames; ++index) {
        seismic_frame_t frame = {
            .timestamp_us = (int64_t)(1000U + index * 2000U),
            .channel_uv = {(float)index, 2.0f, 3.0f, 4.0f},
        };
        assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    }

    const unsigned char tail[16] = {0xA5U};
    assert(trailing_bytes <= sizeof(tail));
    if (trailing_bytes > 0U) {
        assert(fwrite(tail, 1U, trailing_bytes, file) == trailing_bytes);
    }
    assert(fflush(file) == 0);
    assert(fclose(file) == 0);
}

static void assert_recovered(size_t expected_frames)
{
    size_t recovered = 99U;
    assert(
        seismic_recorder_recover_object_sessions(
            "OBJ_0001",
            NULL,
            NULL,
            &recovered
        ) == ESP_OK
    );
    assert(recovered == 1U);
    assert(fake_metadata_write_calls == 1U);
    assert(strcmp(fake_last_metadata_status, "recovered_error") == 0);
    assert(fake_last_metadata_snapshot.samples_written == expected_frames);
    assert(fake_last_metadata_snapshot.cadence.available == false);
    assert(fake_terminal_status == SEISMIC_RECORDER_SESSION_RECOVERED_ERROR);
    assert(fake_storage_begin_calls == fake_storage_end_calls);
}

static void test_partial_tail_salvage_and_idempotence(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(2U, 7U, RAW_HEADER_VALID);

    const uint64_t expected = sizeof(seismic_recorder_binary_header_t) + 2U * sizeof(seismic_frame_t);
    assert(samples_file_size() == expected + 7U);
    assert_recovered(2U);
    assert(samples_file_size() == expected);

    size_t recovered = 99U;
    assert(seismic_recorder_recover_object_sessions("OBJ_0001", NULL, NULL, &recovered) == ESP_OK);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 1U);
    assert(samples_file_size() == expected);
    reset_layout();
}


static void test_v2_metadata_is_recovered_and_v3_is_rejected(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata_v2());
    write_raw(1U, 0U, RAW_HEADER_VALID);
    assert_recovered(1U);
    assert(fake_last_participation.pods[0].assigned);
    assert(fake_last_participation.pods[0].node_id_known);
    assert(fake_last_participation.pods[0].node_id == 73U);
    assert(fake_last_participation.pods[0].hardware_id_length == 2U);
    assert(fake_last_participation.pods[0].hardware_id[0] == 0xABU);
    assert(fake_last_participation.pods[0].hardware_id[1] == 0xCDU);
    assert(fake_last_participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    reset_layout();

    make_layout();
    reset_fakes();
    write_candidate("session.json", unsupported_recording_metadata_v3());
    write_raw(1U, 0U, RAW_HEADER_VALID);
    const uint64_t original_size = samples_file_size();
    size_t recovered = 99U;
    assert(seismic_recorder_recover_object_sessions("OBJ_0001", NULL, NULL, &recovered) == ESP_OK);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 0U);
    assert(samples_file_size() == original_size);
    assert(fake_storage_begin_calls == fake_storage_end_calls);
    reset_layout();
}

static void test_backup_and_temp_candidates(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", "{broken");
    write_candidate("session.json.bak", valid_recording_metadata());
    write_raw(1U, 0U, RAW_HEADER_VALID);
    assert_recovered(1U);
    reset_layout();

    make_layout();
    reset_fakes();
    write_candidate("session.json", "{broken");
    write_candidate("session.json.bak", "{also-broken");
    write_candidate("session.json.tmp", valid_recording_metadata());
    write_raw(3U, 0U, RAW_HEADER_VALID);
    assert_recovered(3U);
    reset_layout();
}

static void test_header_only_recovers_zero_frames(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(0U, 0U, RAW_HEADER_VALID);
    assert_recovered(0U);
    assert(samples_file_size() == sizeof(seismic_recorder_binary_header_t));
    reset_layout();
}

static void test_invalid_headers_fail_closed(void)
{
    const raw_header_variant_t invalid[] = {
        RAW_HEADER_BAD_MAGIC,
        RAW_HEADER_BAD_VERSION,
        RAW_HEADER_BAD_CHANNEL_COUNT,
        RAW_HEADER_BAD_FRAME_SIZE,
    };

    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        make_layout();
        reset_fakes();
        write_candidate("session.json", valid_recording_metadata());
        write_raw(2U, 5U, invalid[index]);
        const uint64_t original_size = samples_file_size();

        size_t recovered = 99U;
        assert(seismic_recorder_recover_object_sessions("OBJ_0001", NULL, NULL, &recovered) == ESP_OK);
        assert(recovered == 0U);
        assert(fake_metadata_write_calls == 0U);
        assert(samples_file_size() == original_size);
        assert(fake_storage_begin_calls == fake_storage_end_calls);
        reset_layout();
    }
}

static void test_live_and_existing_terminal_sessions_are_authoritative(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(2U, 9U, RAW_HEADER_VALID);
    const uint64_t original_size = samples_file_size();

    fake_live_snapshot.running = true;
    snprintf(fake_live_snapshot.object_id, sizeof(fake_live_snapshot.object_id), "OBJ_0001");
    snprintf(fake_live_snapshot.session_id, sizeof(fake_live_snapshot.session_id), "S000001");

    size_t recovered = 99U;
    assert(seismic_recorder_recover_object_sessions("OBJ_0001", NULL, NULL, &recovered) == ESP_OK);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 0U);
    assert(samples_file_size() == original_size);
    reset_layout();

    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(2U, 3U, RAW_HEADER_VALID);
    const uint64_t terminal_original_size = samples_file_size();
    fake_terminal_present = true;
    fake_terminal_status = SEISMIC_RECORDER_SESSION_ERROR;
    fake_last_metadata_snapshot.samples_written = 1U;

    recovered = 99U;
    assert(seismic_recorder_recover_object_sessions("OBJ_0001", NULL, NULL, &recovered) == ESP_OK);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 0U);
    assert(samples_file_size() == terminal_original_size);
    reset_layout();
}

static bool stop_immediately(void *context)
{
    unsigned *calls = (unsigned *)context;
    ++(*calls);
    return true;
}

static void test_cooperative_stop_does_not_touch_session(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(2U, 7U, RAW_HEADER_VALID);
    const uint64_t original_size = samples_file_size();
    unsigned stop_calls = 0U;
    size_t recovered = 99U;

    assert(
        seismic_recorder_recover_object_sessions(
            "OBJ_0001",
            stop_immediately,
            &stop_calls,
            &recovered
        ) == ESP_OK
    );
    assert(stop_calls == 1U);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 0U);
    assert(samples_file_size() == original_size);
    reset_layout();
}

static void test_candidate_allocation_failure_leaves_files_untouched(void)
{
    make_layout();
    reset_fakes();
    write_candidate("session.json", valid_recording_metadata());
    write_raw(2U, 7U, RAW_HEADER_VALID);

    char metadata_path[256];
    char samples_path[256];
    assert(snprintf(
        metadata_path,
        sizeof(metadata_path),
        "%s/OBJ_0001/sessions/S000001/session.json",
        STORAGE_MANAGER_OBJECTS_PATH
    ) < (int)sizeof(metadata_path));
    assert(snprintf(
        samples_path,
        sizeof(samples_path),
        "%s/OBJ_0001/sessions/S000001/samples.bin",
        STORAGE_MANAGER_OBJECTS_PATH
    ) < (int)sizeof(samples_path));

    unsigned char metadata_before[256];
    unsigned char samples_before[128];
    const size_t metadata_size = read_file_bytes(
        metadata_path,
        metadata_before,
        sizeof(metadata_before)
    );
    const size_t samples_size = read_file_bytes(
        samples_path,
        samples_before,
        sizeof(samples_before)
    );
    size_t recovered = 99U;
    fail_recovery_candidate_malloc = true;
    assert(
        seismic_recorder_recover_object_sessions(
            "OBJ_0001",
            NULL,
            NULL,
            &recovered
        ) == ESP_ERR_NO_MEM
    );
    assert(!fail_recovery_candidate_malloc);
    assert(recovered == 0U);
    assert(fake_metadata_write_calls == 0U);

    unsigned char metadata_after[sizeof(metadata_before)];
    unsigned char samples_after[sizeof(samples_before)];
    assert(read_file_bytes(metadata_path, metadata_after, sizeof(metadata_after)) == metadata_size);
    assert(read_file_bytes(samples_path, samples_after, sizeof(samples_after)) == samples_size);
    assert(memcmp(metadata_after, metadata_before, metadata_size) == 0);
    assert(memcmp(samples_after, samples_before, samples_size) == 0);
    reset_layout();
}

int main(void)
{
    test_partial_tail_salvage_and_idempotence();
    test_v2_metadata_is_recovered_and_v3_is_rejected();
    test_backup_and_temp_candidates();
    test_header_only_recovers_zero_frames();
    test_invalid_headers_fail_closed();
    test_live_and_existing_terminal_sessions_are_authoritative();
    test_cooperative_stop_does_not_touch_session();
    test_candidate_allocation_failure_leaves_files_untouched();
    puts("seismic_recorder_recovery_test: PASS");
    return 0;
}
