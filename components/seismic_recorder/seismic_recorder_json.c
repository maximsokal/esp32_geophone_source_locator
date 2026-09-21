#include "seismic_recorder_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage_manager.h"


#define SEISMIC_RECORDER_SESSION_SCHEMA_VERSION  1

static const char *TAG = "SEISMIC_RECORDER_JSON";

static seismic_recorder_recovery_request_callback_t s_recovery_request_callback = NULL;
static void *s_recovery_request_context = NULL;


esp_err_t seismic_recorder_set_recovery_request_callback(
    seismic_recorder_recovery_request_callback_t callback,
    void *user_context
)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        s_recovery_request_callback != NULL &&
        s_recovery_request_callback != callback
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    s_recovery_request_callback = callback;
    s_recovery_request_context = user_context;
    return ESP_OK;
}


static esp_err_t seismic_recorder_json_metadata_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
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

    const int written = snprintf(
        path,
        path_size,
        "%s/session.json",
        session_path
    );

    return
        written < 0 || written >= (int)path_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


static bool seismic_recorder_json_add_location(
    cJSON *root,
    const object_store_location_t *location
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "base_location");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", location->valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", location->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", location->longitude) != NULL &&
        cJSON_AddNumberToObject(object, "accuracy_m", location->accuracy_m) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", location->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", location->altitude_m) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_accuracy_m", location->altitude_accuracy_m) != NULL &&
        cJSON_AddNumberToObject(object, "captured_at_ms", (double)location->captured_at_ms) != NULL &&
        cJSON_AddStringToObject(object, "source", location->source) != NULL;
}


static bool seismic_recorder_json_add_cables(
    cJSON *root,
    const object_store_profile_t *profile
)
{
    cJSON *array = cJSON_AddArrayToObject(root, "cable_lengths_m");
    if (array == NULL) {
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *number = cJSON_CreateNumber(profile->cable_lengths_m[i]);
        if (number == NULL) {
            return false;
        }
        cJSON_AddItemToArray(array, number);
    }

    return true;
}


/*
 * MAX-166: freeze this recording's as-solved per-pod geometry into the
 * session's own metadata, separate from object.json. object_store_geometry_t
 * on the object can be recomputed/overwritten by a later recording of the
 * same object; this copy is what "this recording's sensor geometry" means
 * from now on, regardless of what the object's live geometry becomes later.
 */
static bool seismic_recorder_json_add_geometry(
    cJSON *root,
    const object_store_geometry_t *geometry
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "geometry");
    if (object == NULL) {
        return false;
    }

    if (cJSON_AddBoolToObject(object, "anchored", geometry->anchored) == NULL) {
        return false;
    }

    cJSON *pods = cJSON_AddArrayToObject(object, "pods");
    if (pods == NULL) {
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        const object_store_pod_geometry_t *pod = &geometry->pods[i];

        cJSON *pod_object = cJSON_CreateObject();
        if (pod_object == NULL) {
            return false;
        }
        cJSON_AddItemToArray(pods, pod_object);

        if (
            cJSON_AddBoolToObject(pod_object, "computed", pod->computed) == NULL ||
            cJSON_AddBoolToObject(pod_object, "leveled", pod->leveled) == NULL ||
            cJSON_AddNumberToObject(pod_object, "x_mm", pod->x_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "y_mm", pod->y_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "z_mm", pod->z_mm) == NULL ||
            cJSON_AddNumberToObject(
                pod_object,
                "computed_at_ms",
                (double)pod->computed_at_ms
            ) == NULL
        ) {
            return false;
        }
    }

    return true;
}


static bool seismic_recorder_json_add_cadence(
    cJSON *root,
    const seismic_recorder_cadence_t *cadence
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "cadence");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "available", cadence->available) != NULL &&
        cJSON_AddNumberToObject(object, "timer_ticks", (double)cadence->timer_ticks) != NULL &&
        cJSON_AddNumberToObject(object, "conversion_attempts", (double)cadence->conversion_attempts) != NULL &&
        cJSON_AddNumberToObject(object, "collapsed_periods", (double)cadence->collapsed_periods) != NULL;
}


static bool seismic_recorder_json_status_is_known(
    const char *status
)
{
    return
        status != NULL &&
        (
            strcmp(status, "recording") == 0 ||
            strcmp(status, "complete") == 0 ||
            strcmp(status, "error") == 0 ||
            strcmp(status, "recovered_error") == 0
        );
}


esp_err_t seismic_recorder_json_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status
)
{
    if (
        profile == NULL ||
        snapshot == NULL ||
        !seismic_recorder_json_status_is_known(status) ||
        snapshot->object_id[0] == '\0' ||
        snapshot->session_id[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool recovered = strcmp(status, "recovered_error") == 0;
    const bool recovered_metrics_available = !recovered;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ok =
        cJSON_AddNumberToObject(
            root,
            "schema_version",
            SEISMIC_RECORDER_SESSION_SCHEMA_VERSION
        ) != NULL &&
        cJSON_AddStringToObject(root, "object_id", snapshot->object_id) != NULL &&
        cJSON_AddStringToObject(root, "object_name", profile->summary.name) != NULL &&
        cJSON_AddStringToObject(root, "session_id", snapshot->session_id) != NULL &&
        cJSON_AddStringToObject(root, "status", status) != NULL &&
        cJSON_AddStringToObject(
            root,
            "stop_stage",
            seismic_recorder_stop_stage_name(snapshot->stop_stage)
        ) != NULL &&
        cJSON_AddStringToObject(
            root,
            "stop_error_stage",
            seismic_recorder_stop_stage_name(snapshot->stop_error_stage)
        ) != NULL &&
        cJSON_AddNumberToObject(
            root,
            "stop_stage_error",
            snapshot->stop_stage_error
        ) != NULL &&
        cJSON_AddStringToObject(root, "source", snapshot->source_name) != NULL &&
        cJSON_AddNumberToObject(root, "sample_rate_hz", snapshot->sample_rate_hz) != NULL &&
        cJSON_AddNumberToObject(root, "channel_count", snapshot->channel_count) != NULL &&
        cJSON_AddNumberToObject(root, "samples_written", (double)snapshot->samples_written) != NULL &&
        cJSON_AddBoolToObject(
            root,
            "dropped_frames_available",
            recovered_metrics_available
        ) != NULL &&
        cJSON_AddNumberToObject(root, "dropped_frames", (double)snapshot->dropped_frames) != NULL &&
        cJSON_AddBoolToObject(
            root,
            "recording_ms_available",
            recovered_metrics_available
        ) != NULL &&
        cJSON_AddNumberToObject(root, "recording_ms", (double)snapshot->recording_ms) != NULL &&
        cJSON_AddNumberToObject(root, "last_error", snapshot->last_error) != NULL;

    if (ok) {
        ok = seismic_recorder_json_add_cadence(root, &snapshot->cadence);
    }

    cJSON *format = NULL;
    if (ok) {
        format = cJSON_AddObjectToObject(root, "sample_format");
        ok = format != NULL;
    }

    if (ok) {
        ok =
            cJSON_AddStringToObject(format, "container", "GEOSEIS1") != NULL &&
            cJSON_AddStringToObject(format, "file", "samples.bin") != NULL &&
            cJSON_AddStringToObject(format, "timestamp", "int64_le_us") != NULL &&
            cJSON_AddStringToObject(format, "channels", "float32_le_uv") != NULL &&
            cJSON_AddNumberToObject(format, "frame_size_bytes", sizeof(seismic_frame_t)) != NULL;
    }

    if (ok) {
        ok = seismic_recorder_json_add_location(root, &profile->location);
    }
    if (ok) {
        ok = seismic_recorder_json_add_cables(root, profile);
    }
    if (ok) {
        ok = seismic_recorder_json_add_geometry(root, &profile->geometry);
    }

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        cJSON_free(json);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_json_metadata_path(
        snapshot->object_id,
        snapshot->session_id,
        path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    const int64_t write_started_us =
        esp_timer_get_time();
    if (err == ESP_OK) {
        err = storage_manager_atomic_write_text(path, json);
    }

    ESP_LOGI(
        TAG,
        "JSON_TIMING op=metadata-atomic-write result=%d elapsed_us=%lld",
        (int)err,
        (long long)(esp_timer_get_time() - write_started_us)
    );

    if (
        err != ESP_OK &&
        s_recovery_request_callback != NULL &&
        (
            strcmp(status, "complete") == 0 ||
            strcmp(status, "error") == 0
        )
    ) {
        s_recovery_request_callback(
            snapshot->object_id,
            snapshot->session_id,
            s_recovery_request_context
        );
    }

    free(path);
    cJSON_free(json);
    return err;
}


static bool seismic_recorder_json_uint64(
    const cJSON *root,
    const char *name,
    uint64_t *value
)
{
    if (value == NULL) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (
        !cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble >= 18446744073709551616.0 ||
        floor(item->valuedouble) != item->valuedouble
    ) {
        return false;
    }

    *value = (uint64_t)item->valuedouble;
    return true;
}


static bool seismic_recorder_json_uint32(
    const cJSON *root,
    const char *name,
    uint32_t *value
)
{
    uint64_t parsed = 0U;
    if (
        value == NULL ||
        !seismic_recorder_json_uint64(root, name, &parsed) ||
        parsed > UINT32_MAX
    ) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}


static bool seismic_recorder_json_string_equals(
    const cJSON *root,
    const char *name,
    const char *expected
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return
        cJSON_IsString(item) &&
        item->valuestring != NULL &&
        strcmp(item->valuestring, expected) == 0;
}


static bool seismic_recorder_json_optional_bool(
    const cJSON *root,
    const char *name,
    bool default_value,
    bool *value
)
{
    if (root == NULL || name == NULL || value == NULL) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) {
        *value = default_value;
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *value = cJSON_IsTrue(item);
    return true;
}


static bool seismic_recorder_json_read_cadence(
    const cJSON *root,
    seismic_recorder_cadence_t *cadence
)
{
    if (root == NULL || cadence == NULL) {
        return false;
    }

    memset(cadence, 0, sizeof(*cadence));
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "cadence");
    if (object == NULL) {
        /* Schema v1 predates cadence evidence. Absence is unknown, not zero. */
        return true;
    }
    if (!cJSON_IsObject(object)) {
        return false;
    }

    const cJSON *available =
        cJSON_GetObjectItemCaseSensitive(object, "available");
    if (!cJSON_IsBool(available)) {
        return false;
    }
    if (!cJSON_IsTrue(available)) {
        return true;
    }

    uint64_t timer_ticks = 0U;
    uint64_t conversion_attempts = 0U;
    uint64_t collapsed_periods = 0U;
    if (
        !seismic_recorder_json_uint64(object, "timer_ticks", &timer_ticks) ||
        !seismic_recorder_json_uint64(object, "conversion_attempts", &conversion_attempts) ||
        !seismic_recorder_json_uint64(object, "collapsed_periods", &collapsed_periods) ||
        conversion_attempts > timer_ticks ||
        collapsed_periods != timer_ticks - conversion_attempts
    ) {
        return false;
    }

    cadence->available = true;
    cadence->timer_ticks = timer_ticks;
    cadence->conversion_attempts = conversion_attempts;
    cadence->collapsed_periods = collapsed_periods;
    return true;
}


static esp_err_t seismic_recorder_json_read_summary_impl(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary,
    bool primary_known_present
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        summary == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary, 0, sizeof(*summary));

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_json_metadata_path(
        object_id,
        session_id,
        path,
        SEISMIC_RECORDER_PATH_SIZE
    );
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    char *buffer = malloc(SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    if (buffer == NULL) {
        free(path);
        return ESP_ERR_NO_MEM;
    }

    err = primary_known_present
        ? storage_manager_read_text_known_present(
            path,
            buffer,
            SEISMIC_RECORDER_METADATA_BUFFER_SIZE
        )
        : storage_manager_read_text(
            path,
            buffer,
            SEISMIC_RECORDER_METADATA_BUFFER_SIZE
        );
    if (err != ESP_OK) {
        free(buffer);
        free(path);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        free(path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool valid =
        seismic_recorder_json_string_equals(
            root,
            "object_id",
            object_id
        ) &&
        seismic_recorder_json_string_equals(
            root,
            "session_id",
            session_id
        );

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    valid = valid &&
        cJSON_IsNumber(schema) &&
        isfinite(schema->valuedouble) &&
        schema->valuedouble == SEISMIC_RECORDER_SESSION_SCHEMA_VERSION;

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const bool complete =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "complete") == 0;
    const bool errored =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "error") == 0;
    const bool recovered =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "recovered_error") == 0;
    valid = valid && (complete || errored || recovered);

    const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
    valid = valid &&
        cJSON_IsString(source) &&
        source->valuestring != NULL &&
        source->valuestring[0] != '\0' &&
        strnlen(source->valuestring, SEISMIC_RECORDER_SOURCE_NAME_SIZE) <
            SEISMIC_RECORDER_SOURCE_NAME_SIZE;

    uint32_t sample_rate_hz = 0U;
    valid = valid &&
        seismic_recorder_json_uint32(
            root,
            "sample_rate_hz",
            &sample_rate_hz
        ) &&
        sample_rate_hz > 0U;

    uint32_t channel_count = 0U;
    valid = valid &&
        seismic_recorder_json_uint32(
            root,
            "channel_count",
            &channel_count
        ) &&
        channel_count == SEISMIC_CHANNEL_COUNT;

    uint64_t samples_written = 0U;
    uint64_t dropped_frames = 0U;
    uint64_t recording_ms = 0U;
    bool dropped_frames_available = true;
    bool recording_ms_available = true;
    seismic_recorder_cadence_t cadence = {0};
    valid = valid &&
        seismic_recorder_json_uint64(root, "samples_written", &samples_written) &&
        seismic_recorder_json_uint64(root, "dropped_frames", &dropped_frames) &&
        seismic_recorder_json_uint64(root, "recording_ms", &recording_ms) &&
        seismic_recorder_json_optional_bool(
            root,
            "dropped_frames_available",
            true,
            &dropped_frames_available
        ) &&
        seismic_recorder_json_optional_bool(
            root,
            "recording_ms_available",
            true,
            &recording_ms_available
        ) &&
        seismic_recorder_json_read_cadence(root, &cadence);

    if (
        recovered &&
        (dropped_frames_available || recording_ms_available || cadence.available)
    ) {
        valid = false;
    }

    if (!valid) {
        cJSON_Delete(root);
        free(path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    snprintf(
        summary->session_id,
        sizeof(summary->session_id),
        "%s",
        session_id
    );
    snprintf(
        summary->source_name,
        sizeof(summary->source_name),
        "%s",
        source->valuestring
    );
    summary->status = complete
        ? SEISMIC_RECORDER_SESSION_COMPLETE
        : recovered
            ? SEISMIC_RECORDER_SESSION_RECOVERED_ERROR
            : SEISMIC_RECORDER_SESSION_ERROR;
    summary->complete = complete;
    summary->readable = true;
    summary->recovered = recovered;
    summary->dropped_frames_available = dropped_frames_available;
    summary->recording_ms_available = recording_ms_available;
    summary->sample_rate_hz = sample_rate_hz;
    summary->channel_count = (uint8_t)channel_count;
    summary->samples_written = samples_written;
    summary->dropped_frames = dropped_frames;
    summary->recording_ms = recording_ms;
    summary->cadence = cadence;

    cJSON_Delete(root);
    free(path);
    return ESP_OK;
}


esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary_impl(
        object_id,
        session_id,
        summary,
        false
    );
}


esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary_impl(
        object_id,
        session_id,
        summary,
        true
    );
}
