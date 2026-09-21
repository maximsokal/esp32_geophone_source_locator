#include "web_server_internal.h"

#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "recording_gate.h"
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
static const char *web_status_measurement_state_name(
    measurement_service_state_t state
)
{
    switch (state) {
        case MEASUREMENT_SERVICE_STARTING:
            return "STARTING";
        case MEASUREMENT_SERVICE_RECORDING:
            return "RECORDING";
        case MEASUREMENT_SERVICE_STOPPING:
            return "STOPPING";
        case MEASUREMENT_SERVICE_REVIEW:
            return "REVIEW";
        case MEASUREMENT_SERVICE_DELETING:
            return "DELETING";
        case MEASUREMENT_SERVICE_ERROR:
            return "ERROR";
        case MEASUREMENT_SERVICE_LIVE:
        default:
            return "LIVE";
    }
}


static const char *web_status_recording_gate_decision_name(
    recording_gate_decision_t decision
)
{
    switch (decision) {
        case RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES:
            return "BLOCKED_INSUFFICIENT_REMOTES";
        case RECORDING_GATE_BLOCKED_NOT_LEVEL:
            return "BLOCKED_NOT_LEVEL";
        case RECORDING_GATE_ALLOWED_PLANAR_2D:
            return "ALLOWED_PLANAR_2D";
        case RECORDING_GATE_ALLOWED_3D_CAPABLE:
        default:
            return "ALLOWED_3D_CAPABLE";
    }
}


static const char *web_status_dimensionality_name(
    localization_dimensionality_t dimensionality
)
{
    switch (dimensionality) {
        case LOCALIZATION_DIMENSIONALITY_INSUFFICIENT:
            return "INSUFFICIENT";
        case LOCALIZATION_DIMENSIONALITY_PLANAR_2D:
            return "PLANAR_2D";
        case LOCALIZATION_DIMENSIONALITY_CAPABLE_3D:
        default:
            return "CAPABLE_3D";
    }
}


/*
 * MAX-165: the same decision seismic_recorder_start() itself enforces
 * (recording_gate_evaluate(), reusing nrf_topology_service's own leveling
 * summary), surfaced ahead of time so the WebUI Record button can disable
 * itself and show the exact required reason/warning text instead of the
 * operator only discovering a rejection after pressing Record.
 */
bool web_status_json_add_recording_gate(cJSON *root)
{
    cJSON *object = cJSON_AddObjectToObject(root, "recording_gate");
    if (object == NULL) {
        return false;
    }

    nrf_topology_leveling_summary_t leveling;
    const esp_err_t err = nrf_topology_service_get_leveling_summary(&leveling);

    if (cJSON_AddBoolToObject(object, "available", err == ESP_OK) == NULL) {
        return false;
    }

    if (err != ESP_OK) {
        return cJSON_AddStringToObject(object, "error", esp_err_to_name(err)) != NULL;
    }

    const recording_gate_result_t gate =
        recording_gate_evaluate(leveling.assigned_count, leveling.accepted_count);

    return
        cJSON_AddNumberToObject(object, "assigned_count", (double)gate.assigned_count) != NULL &&
        cJSON_AddNumberToObject(object, "accepted_count", (double)gate.accepted_count) != NULL &&
        cJSON_AddStringToObject(
            object,
            "decision",
            web_status_recording_gate_decision_name(gate.decision)
        ) != NULL &&
        cJSON_AddStringToObject(
            object,
            "dimensionality",
            web_status_dimensionality_name(gate.dimensionality)
        ) != NULL &&
        cJSON_AddBoolToObject(
            object,
            "allowed",
            gate.decision == RECORDING_GATE_ALLOWED_PLANAR_2D ||
                gate.decision == RECORDING_GATE_ALLOWED_3D_CAPABLE
        ) != NULL &&
        cJSON_AddStringToObject(
            object,
            "reason",
            recording_gate_decision_reason(gate.decision)
        ) != NULL &&
        cJSON_AddStringToObject(
            object,
            "warning",
            recording_gate_planar_warning(gate.decision)
        ) != NULL;
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
bool web_status_json_add_measurement(cJSON *root)
{
    cJSON *object = cJSON_AddObjectToObject(root, "measurement");
    if (object == NULL) {
        return false;
    }

    measurement_service_snapshot_t snapshot;
    const esp_err_t err = measurement_service_get_snapshot(&snapshot);

    if (cJSON_AddBoolToObject(object, "available", err == ESP_OK) == NULL) {
        return false;
    }

    if (err != ESP_OK) {
        return cJSON_AddStringToObject(object, "error", esp_err_to_name(err)) != NULL;
    }

    if (
        cJSON_AddBoolToObject(object, "initialized", snapshot.initialized) == NULL ||
        cJSON_AddBoolToObject(object, "busy", snapshot.busy) == NULL ||
        cJSON_AddBoolToObject(object, "active_object_present", snapshot.active_object_present) == NULL ||
        cJSON_AddStringToObject(
            object,
            "state",
            web_status_measurement_state_name(snapshot.state)
        ) == NULL ||
        cJSON_AddStringToObject(object, "object_id", snapshot.object_id) == NULL ||
        cJSON_AddStringToObject(object, "object_name", snapshot.object_name) == NULL ||
        cJSON_AddStringToObject(object, "session_id", snapshot.session_id) == NULL ||
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
        cJSON_AddNumberToObject(object, "stop_stage_error", snapshot.stop_stage_error) == NULL ||
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
