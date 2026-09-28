#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "cJSON.h"
#include "esp_log.h"
#include "object_format.h"
#include "object_store.h"
#include "storage_manager.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


static const char *TAG = "SEISMIC_RECOVERY";


typedef struct
{
    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];
    uint32_t sample_rate_hz;
    uint8_t channel_count;
    seismic_recorder_session_participation_t participation;

} seismic_recorder_recovery_candidate_t;


static bool seismic_recorder_recovery_session_id_valid(
    const char *session_id
)
{
    if (session_id == NULL || strlen(session_id) != 7U || session_id[0] != 'S') {
        return false;
    }

    unsigned value = 0U;
    for (size_t index = 1U; index < 7U; ++index) {
        if (session_id[index] < '0' || session_id[index] > '9') {
            return false;
        }
        value = value * 10U + (unsigned)(session_id[index] - '0');
    }

    return value > 0U;
}


static bool seismic_recorder_recovery_live_session(
    const char *object_id,
    const char *session_id
)
{
    seismic_recorder_snapshot_t snapshot;
    if (seismic_recorder_get_snapshot(&snapshot) != ESP_OK) {
        return false;
    }

    return
        snapshot.running &&
        strcmp(snapshot.object_id, object_id) == 0 &&
        strcmp(snapshot.session_id, session_id) == 0;
}


static esp_err_t seismic_recorder_recovery_build_path(
    const char *session_path,
    const char *file_name,
    char *path,
    size_t path_size
)
{
    if (
        session_path == NULL ||
        file_name == NULL ||
        path == NULL ||
        path_size == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(path, path_size, "%s/%s", session_path, file_name);
    return written < 0 || written >= (int)path_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}


static esp_err_t seismic_recorder_recovery_read_text_exact(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    if (path == NULL || buffer == NULL || buffer_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        const int open_errno = errno;
        storage_manager_operation_end();
        return open_errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool read_error = ferror(file) != 0;
    const bool too_large = count == buffer_size - 1U && !feof(file);
    const int close_result = fclose(file);
    storage_manager_operation_end();

    if (read_error || close_result != 0) {
        return ESP_FAIL;
    }
    if (too_large) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[count] = '\0';
    return ESP_OK;
}


static bool seismic_recorder_recovery_json_uint32(
    const cJSON *root,
    const char *name,
    uint32_t *value
)
{
    if (root == NULL || name == NULL || value == NULL) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (
        !cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX ||
        floor(item->valuedouble) != item->valuedouble
    ) {
        return false;
    }

    *value = (uint32_t)item->valuedouble;
    return true;
}


static bool seismic_recorder_recovery_status_candidate(
    const cJSON *status
)
{
    if (!cJSON_IsString(status) || status->valuestring == NULL) {
        return false;
    }

    return
        strcmp(status->valuestring, "recording") == 0 ||
        strcmp(status->valuestring, "complete") == 0 ||
        strcmp(status->valuestring, "error") == 0 ||
        strcmp(status->valuestring, "recovered_error") == 0;
}


static esp_err_t seismic_recorder_recovery_parse_candidate(
    const char *json,
    const char *object_id,
    const char *session_id,
    seismic_recorder_recovery_candidate_t *candidate
)
{
    if (
        json == NULL ||
        object_id == NULL ||
        session_id == NULL ||
        candidate == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(candidate, 0, sizeof(*candidate));
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "object_id");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");

    uint32_t sample_rate_hz = 0U;
    uint32_t channel_count = 0U;
    bool valid =
        cJSON_IsNumber(schema) &&
        isfinite(schema->valuedouble) &&
        (
            schema->valuedouble == SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V1 ||
            schema->valuedouble == SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V2
        ) &&
        cJSON_IsString(object) &&
        object->valuestring != NULL &&
        strcmp(object->valuestring, object_id) == 0 &&
        cJSON_IsString(session) &&
        session->valuestring != NULL &&
        strcmp(session->valuestring, session_id) == 0 &&
        seismic_recorder_recovery_status_candidate(status) &&
        cJSON_IsString(source) &&
        source->valuestring != NULL &&
        source->valuestring[0] != '\0' &&
        strnlen(source->valuestring, SEISMIC_RECORDER_SOURCE_NAME_SIZE) <
            SEISMIC_RECORDER_SOURCE_NAME_SIZE &&
        seismic_recorder_recovery_json_uint32(
            root,
            "sample_rate_hz",
            &sample_rate_hz
        ) &&
        sample_rate_hz > 0U &&
        seismic_recorder_recovery_json_uint32(
            root,
            "channel_count",
            &channel_count
        ) &&
        channel_count == SEISMIC_CHANNEL_COUNT;

    if (
        valid &&
        seismic_recorder_json_parse_participation_text(
            json,
            &candidate->participation
        ) != ESP_OK
    ) {
        valid = false;
    }

    if (valid) {
        snprintf(
            candidate->source_name,
            sizeof(candidate->source_name),
            "%s",
            source->valuestring
        );
        candidate->sample_rate_hz = sample_rate_hz;
        candidate->channel_count = (uint8_t)channel_count;
    }

    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}


static esp_err_t seismic_recorder_recovery_find_candidate(
    const char *session_path,
    const char *object_id,
    const char *session_id,
    seismic_recorder_recovery_candidate_t *candidate
)
{
    static const char *const names[] = {
        "session.json",
        "session.json.bak",
        "session.json.tmp",
    };

    char path[SEISMIC_RECORDER_PATH_SIZE];
    char *buffer = malloc(SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool saw_candidate_file = false;

    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        esp_err_t err = seismic_recorder_recovery_build_path(
            session_path,
            names[index],
            path,
            sizeof(path)
        );
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }

        err = seismic_recorder_recovery_read_text_exact(
            path,
            buffer,
            SEISMIC_RECORDER_METADATA_BUFFER_SIZE
        );
        if (err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            saw_candidate_file = true;
            continue;
        }

        saw_candidate_file = true;
        err = seismic_recorder_recovery_parse_candidate(
            buffer,
            object_id,
            session_id,
            candidate
        );
        if (err == ESP_OK) {
            free(buffer);
            return ESP_OK;
        }
    }

    free(buffer);
    return saw_candidate_file ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FOUND;
}


static int seismic_recorder_recovery_file_descriptor(FILE *file)
{
#ifdef _WIN32
    return _fileno(file);
#else
    return fileno(file);
#endif
}


static int seismic_recorder_recovery_sync_descriptor(
    int descriptor
)
{
#ifdef _WIN32
    return _commit(descriptor);
#else
    return fsync(descriptor);
#endif
}


static int seismic_recorder_recovery_truncate_descriptor(
    int descriptor,
    uint64_t size_bytes
)
{
#ifdef _WIN32
    return _chsize_s(descriptor, size_bytes) == 0 ? 0 : -1;
#else
    if (size_bytes > (uint64_t)LLONG_MAX) {
        errno = EFBIG;
        return -1;
    }
    return ftruncate(descriptor, (off_t)size_bytes);
#endif
}


static esp_err_t seismic_recorder_recovery_validate_raw(
    const char *samples_path,
    const seismic_recorder_recovery_candidate_t *candidate,
    uint64_t *frame_count,
    uint16_t *format_version
)
{
    if (
        samples_path == NULL || candidate == NULL || frame_count == NULL ||
        format_version == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_count = 0U;
    *format_version = 0U;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(samples_path, "rb+");
    if (file == NULL) {
        const int open_errno = errno;
        storage_manager_operation_end();
        return open_errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        err = ESP_FAIL;
    }

    long end_position = -1L;
    if (err == ESP_OK) {
        end_position = ftell(file);
        if (end_position < 0L) {
            err = ESP_FAIL;
        }
    }

    uint64_t whole_frames = 0U;
    uint64_t target_size = 0U;
    uint16_t recovered_format_version = 0U;
    if (err == ESP_OK) {
        err = seismic_recorder_samples_recover_file(
            file,
            (uint64_t)end_position,
            candidate->sample_rate_hz,
            candidate->channel_count,
            &whole_frames,
            &target_size,
            &recovered_format_version
        );
    }

    const int descriptor = err == ESP_OK
        ? seismic_recorder_recovery_file_descriptor(file)
        : -1;
    if (err == ESP_OK && descriptor < 0) {
        err = ESP_FAIL;
    }

    if (
        err == ESP_OK &&
        target_size != (uint64_t)end_position &&
        seismic_recorder_recovery_truncate_descriptor(
            descriptor,
            target_size
        ) != 0
    ) {
        err = ESP_FAIL;
    }

    /* Establish the raw file as durable before terminal recovered metadata is
     * allowed to point at it. This is required even when no partial tail was
     * present: same-runtime recovery can follow a metadata-only stop failure. */
    if (
        err == ESP_OK &&
        seismic_recorder_recovery_sync_descriptor(descriptor) != 0
    ) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK && fseek(file, 0L, SEEK_END) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        const long verified_position = ftell(file);
        if (
            verified_position < 0L ||
            (uint64_t)verified_position != target_size
        ) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    storage_manager_operation_end();

    if (err == ESP_OK) {
        *frame_count = whole_frames;
        *format_version = recovered_format_version;
    }
    return err;
}


static esp_err_t seismic_recorder_recovery_one(
    const object_store_profile_t *profile,
    const char *object_id,
    const char *session_id,
    bool *recovered
)
{
    if (
        profile == NULL ||
        object_id == NULL ||
        session_id == NULL ||
        recovered == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *recovered = false;

    if (seismic_recorder_recovery_live_session(object_id, session_id)) {
        return ESP_OK;
    }

    seismic_recorder_session_summary_t terminal_summary;
    if (
        seismic_recorder_json_read_summary(
            object_id,
            session_id,
            &terminal_summary
        ) == ESP_OK
    ) {
        return ESP_OK;
    }

    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    esp_err_t err = seismic_recorder_fs_build_session_path(
        object_id,
        session_id,
        session_path,
        sizeof(session_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    seismic_recorder_recovery_candidate_t candidate;
    err = seismic_recorder_recovery_find_candidate(
        session_path,
        object_id,
        session_id,
        &candidate
    );
    if (err != ESP_OK) {
        return err;
    }

    if (seismic_recorder_recovery_live_session(object_id, session_id)) {
        return ESP_OK;
    }

    char samples_path[SEISMIC_RECORDER_PATH_SIZE];
    err = seismic_recorder_recovery_build_path(
        session_path,
        "samples.bin",
        samples_path,
        sizeof(samples_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    uint64_t frame_count = 0U;
    uint16_t format_version = 0U;
    err = seismic_recorder_recovery_validate_raw(
        samples_path,
        &candidate,
        &frame_count,
        &format_version
    );
    if (err != ESP_OK) {
        return err;
    }

    if (seismic_recorder_recovery_live_session(object_id, session_id)) {
        return ESP_OK;
    }

    seismic_recorder_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.initialized = true;
    snapshot.running = false;
    snapshot.state = SEISMIC_RECORDER_ERROR;
    snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_ERROR;
    snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    snapshot.sample_rate_hz = candidate.sample_rate_hz;
    snapshot.channel_count = candidate.channel_count;
    snapshot.format_version = format_version;
    snapshot.samples_written = frame_count;
    snapshot.last_error = ESP_FAIL;
    snapshot.stop_stage_error = ESP_OK;
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "%s", object_id);
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "%s", session_id);
    snprintf(
        snapshot.source_name,
        sizeof(snapshot.source_name),
        "%s",
        candidate.source_name
    );

    err = seismic_recorder_json_write_metadata(
        profile,
        &snapshot,
        "recovered_error",
        &candidate.participation
    );
    if (err != ESP_OK) {
        return err;
    }

    seismic_recorder_session_summary_t verify_summary;
    err = seismic_recorder_json_read_summary(
        object_id,
        session_id,
        &verify_summary
    );
    if (
        err != ESP_OK ||
        verify_summary.status != SEISMIC_RECORDER_SESSION_RECOVERED_ERROR ||
        !verify_summary.readable ||
        verify_summary.complete ||
        verify_summary.samples_written != frame_count
    ) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    *recovered = true;
    ESP_LOGW(
        TAG,
        "Recovered stale session object=%s session=%s frames=%llu status=recovered_error",
        object_id,
        session_id,
        (unsigned long long)frame_count
    );
    return ESP_OK;
}


esp_err_t seismic_recorder_recover_object_sessions(
    const char *object_id,
    seismic_recorder_recovery_stop_requested_fn stop_requested,
    void *user_context,
    size_t *recovered_count
)
{
    if (!object_format_id_is_valid(object_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (recovered_count != NULL) {
        *recovered_count = 0U;
    }

    object_store_profile_t profile;
    esp_err_t err = object_store_get_profile(object_id, &profile);
    if (err != ESP_OK) {
        return err;
    }

    char sessions_path[SEISMIC_RECORDER_PATH_SIZE];
    const int written = snprintf(
        sessions_path,
        sizeof(sessions_path),
        "%s/%s/sessions",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id
    );
    if (written < 0 || written >= (int)sizeof(sessions_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char session_ids[SEISMIC_RECORDER_MAX_SESSIONS][SEISMIC_RECORDER_SESSION_ID_SIZE];
    size_t session_count = 0U;

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory = opendir(sessions_path);
    if (directory == NULL) {
        const int open_errno = errno;
        storage_manager_operation_end();
        return open_errno == ENOENT || open_errno == ENOTDIR
            ? ESP_OK
            : ESP_FAIL;
    }

    errno = 0;
    struct dirent *entry = NULL;
    while (
        session_count < SEISMIC_RECORDER_MAX_SESSIONS &&
        (entry = readdir(directory)) != NULL
    ) {
        if (!seismic_recorder_recovery_session_id_valid(entry->d_name)) {
            continue;
        }
        memcpy(
            session_ids[session_count],
            entry->d_name,
            sizeof("S000000")
        );
        ++session_count;
    }

    const int read_errno = errno;
    const int close_result = closedir(directory);
    storage_manager_operation_end();
    if (read_errno != 0 || close_result != 0) {
        return ESP_FAIL;
    }

    esp_err_t first_operational_error = ESP_OK;
    for (size_t index = 0U; index < session_count; ++index) {
        if (stop_requested != NULL && stop_requested(user_context)) {
            break;
        }

        bool recovered = false;
        const esp_err_t session_err = seismic_recorder_recovery_one(
            &profile,
            object_id,
            session_ids[index],
            &recovered
        );

        if (session_err == ESP_OK) {
            if (recovered && recovered_count != NULL) {
                ++(*recovered_count);
            }
        }
        else if (
            session_err == ESP_ERR_INVALID_RESPONSE ||
            session_err == ESP_ERR_NOT_FOUND ||
            session_err == ESP_ERR_INVALID_SIZE
        ) {
            /* Corrupt/unrecognised evidence is intentionally fail-closed. One
             * bad historical session must not block salvage of the archive. */
            ESP_LOGW(
                TAG,
                "Session not salvageable object=%s session=%s err=%s",
                object_id,
                session_ids[index],
                esp_err_to_name(session_err)
            );
        }
        else if (first_operational_error == ESP_OK) {
            first_operational_error = session_err;
        }

#ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(1));
#endif
    }

    return first_operational_error;
}
