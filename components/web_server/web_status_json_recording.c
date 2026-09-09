#include "web_server_internal.h"

#include <stdlib.h>

#include "seismic_recorder.h"


static const char *web_status_recording_state_name(
    seismic_recorder_state_t state
)
{
    switch (state) {
        case SEISMIC_RECORDER_RECORDING:
            return "RECORDING";
        case SEISMIC_RECORDER_STOPPING:
            return "STOPPING";
        case SEISMIC_RECORDER_ERROR:
            return "ERROR";
        case SEISMIC_RECORDER_IDLE:
        default:
            return "IDLE";
    }
}


bool web_status_json_add_recording(cJSON *root)
{
    cJSON *object = cJSON_AddObjectToObject(root, "recording");
    if (object == NULL) {
        return false;
    }

    seismic_recorder_snapshot_t snapshot;
    const esp_err_t err = seismic_recorder_get_snapshot(&snapshot);

    if (cJSON_AddBoolToObject(object, "available", err == ESP_OK) == NULL) {
        return false;
    }

    if (err != ESP_OK) {
        return cJSON_AddStringToObject(
            object,
            "error",
            esp_err_to_name(err)
        ) != NULL;
    }

    if (
        cJSON_AddBoolToObject(object, "running", snapshot.running) == NULL ||
        cJSON_AddStringToObject(
            object,
            "state",
            web_status_recording_state_name(snapshot.state)
        ) == NULL ||
        cJSON_AddStringToObject(
            object,
            "stop_stage",
            seismic_recorder_stop_stage_name(snapshot.stop_stage)
        ) == NULL ||
        cJSON_AddStringToObject(
            object,
            "stop_error_stage",
            seismic_recorder_stop_stage_name(snapshot.stop_error_stage)
        ) == NULL ||
        cJSON_AddNumberToObject(
            object,
            "stop_stage_error",
            snapshot.stop_stage_error
        ) == NULL ||
        cJSON_AddStringToObject(object, "object_id", snapshot.object_id) == NULL ||
        cJSON_AddStringToObject(object, "session_id", snapshot.session_id) == NULL ||
        cJSON_AddStringToObject(object, "source", snapshot.source_name) == NULL ||
        cJSON_AddNumberToObject(object, "sample_rate_hz", snapshot.sample_rate_hz) == NULL ||
        cJSON_AddNumberToObject(object, "channel_count", snapshot.channel_count) == NULL ||
        cJSON_AddNumberToObject(object, "samples_written", (double)snapshot.samples_written) == NULL ||
        cJSON_AddNumberToObject(object, "dropped_frames", (double)snapshot.dropped_frames) == NULL ||
        cJSON_AddNumberToObject(object, "recording_ms", (double)snapshot.recording_ms) == NULL ||
        cJSON_AddNumberToObject(object, "last_error", snapshot.last_error) == NULL
    ) {
        return false;
    }

    if (snapshot.last_error != ESP_OK) {
        return cJSON_AddStringToObject(
            object,
            "last_error_name",
            esp_err_to_name(snapshot.last_error)
        ) != NULL;
    }

    return true;
}


bool web_status_json_add_sessions(
    cJSON *root,
    const char *object_id
)
{
    if (object_id == NULL || object_id[0] == '\0') {
        return true;
    }

    cJSON *collection = cJSON_AddObjectToObject(root, "recording_sessions");
    if (collection == NULL) {
        return false;
    }

    if (cJSON_AddStringToObject(collection, "object_id", object_id) == NULL) {
        return false;
    }

    seismic_recorder_session_summary_t *sessions = calloc(
        SEISMIC_RECORDER_MAX_SESSIONS,
        sizeof(*sessions)
    );
    if (sessions == NULL) {
        return false;
    }

    size_t count = 0U;
    const esp_err_t err = seismic_recorder_list_sessions(
        object_id,
        sessions,
        SEISMIC_RECORDER_MAX_SESSIONS,
        &count
    );

    if (cJSON_AddBoolToObject(collection, "available", err == ESP_OK) == NULL) {
        free(sessions);
        return false;
    }

    cJSON *array = cJSON_AddArrayToObject(collection, "sessions");
    if (array == NULL) {
        free(sessions);
        return false;
    }

    if (err != ESP_OK) {
        cJSON_AddStringToObject(collection, "error", esp_err_to_name(err));
        free(sessions);
        return true;
    }

    for (size_t i = 0; i < count; ++i) {
        cJSON *session = cJSON_CreateObject();
        if (session == NULL) {
            free(sessions);
            return false;
        }
        cJSON_AddItemToArray(array, session);

        if (
            cJSON_AddStringToObject(session, "session_id", sessions[i].session_id) == NULL ||
            cJSON_AddBoolToObject(session, "complete", sessions[i].complete) == NULL ||
            cJSON_AddStringToObject(session, "source", sessions[i].source_name) == NULL ||
            cJSON_AddNumberToObject(session, "sample_rate_hz", sessions[i].sample_rate_hz) == NULL ||
            cJSON_AddNumberToObject(session, "channel_count", sessions[i].channel_count) == NULL ||
            cJSON_AddNumberToObject(session, "samples_written", (double)sessions[i].samples_written) == NULL ||
            cJSON_AddNumberToObject(session, "dropped_frames", (double)sessions[i].dropped_frames) == NULL ||
            cJSON_AddNumberToObject(session, "recording_ms", (double)sessions[i].recording_ms) == NULL
        ) {
            free(sessions);
            return false;
        }
    }

    free(sessions);
    return true;
}
