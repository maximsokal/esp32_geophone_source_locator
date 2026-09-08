#include "seismic_recorder_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage_manager.h"


#define SEISMIC_RECORDER_SESSION_SCHEMA_VERSION  1

static const char *TAG = "SEISMIC_RECORDER_JSON";


static esp_err_t seismic_recorder_json_metadata_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    int written = snprintf(
        path,
        path_size,
        "%s/%s/sessions/%s/session.json",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id
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


esp_err_t seismic_recorder_json_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status
)
{
    if (
        profile == NULL ||
        snapshot == NULL ||
        status == NULL ||
        snapshot->object_id[0] == '\0' ||
        snapshot->session_id[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

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
        cJSON_AddNumberToObject(root, "dropped_frames", (double)snapshot->dropped_frames) != NULL &&
        cJSON_AddNumberToObject(root, "recording_ms", (double)snapshot->recording_ms) != NULL &&
        cJSON_AddNumberToObject(root, "last_error", snapshot->last_error) != NULL;

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

    free(path);
    cJSON_free(json);
    return err;
}


static uint64_t seismic_recorder_json_uint64(
    const cJSON *root,
    const char *name
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
        return 0U;
    }
    return (uint64_t)item->valuedouble;
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

    err = storage_manager_read_text(
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

    snprintf(
        summary->session_id,
        sizeof(summary->session_id),
        "%s",
        session_id
    );

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    summary->complete =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "complete") == 0;

    const cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate_hz");
    if (cJSON_IsNumber(sample_rate) && sample_rate->valuedouble > 0.0) {
        summary->sample_rate_hz = (uint32_t)sample_rate->valuedouble;
    }

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channel_count");
    if (cJSON_IsNumber(channels) && channels->valuedouble > 0.0) {
        summary->channel_count = (uint8_t)channels->valuedouble;
    }

    summary->samples_written = seismic_recorder_json_uint64(root, "samples_written");
    summary->dropped_frames = seismic_recorder_json_uint64(root, "dropped_frames");
    summary->recording_ms = seismic_recorder_json_uint64(root, "recording_ms");

    cJSON_Delete(root);
    free(path);
    return ESP_OK;
}
