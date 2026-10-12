#include "web_server_internal.h"

#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "recording_gate.h"
#include "seismic_recorder.h"
#if defined(ESP_PLATFORM)
#include "storage_manager.h"
#else
#include <stdbool.h>
bool storage_manager_is_ready(void);
#endif
#include "workflow_service.h"


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
 * MAX-165/MAX-444: expose the diagnostic decision and admission result
 * together. The decision/reason remain visible when the default policy
 * bypasses the gate, while "allowed" reflects the actual start policy.
 */
bool web_status_json_add_recording_gate(cJSON *root)
{
    cJSON *object = cJSON_AddObjectToObject(root, "recording_gate");
    if (object == NULL) {
        return false;
    }

    nrf_topology_leveling_summary_t leveling;
    const esp_err_t err = nrf_topology_service_get_leveling_summary(&leveling);
    const bool storage_available = storage_manager_is_ready();

    if (cJSON_AddBoolToObject(object, "available", err == ESP_OK) == NULL) {
        return false;
    }

    if (err != ESP_OK) {
        if (cJSON_AddStringToObject(object, "error", esp_err_to_name(err)) == NULL) {
            return false;
        }
        return storage_available ||
            (cJSON_AddBoolToObject(object, "allowed", false) != NULL &&
             cJSON_AddStringToObject(object, "reason", "STORAGE_UNAVAILABLE") != NULL);
    }

    const recording_gate_result_t gate =
        recording_gate_evaluate(leveling.assigned_count, leveling.accepted_count);

    /* MAX-114: which node blocks the gate, and by how much it is tilted. */
    nrf_topology_leveling_nodes_t nodes;
    if (nrf_topology_service_get_leveling_nodes(&nodes) == ESP_OK) {
        cJSON *list = cJSON_AddArrayToObject(object, "nodes");
        if (list == NULL) {
            return false;
        }
        for (size_t i = 0; i < nodes.count; ++i) {
            const nrf_topology_leveling_node_t *node = &nodes.nodes[i];
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) {
                return false;
            }
            if (!cJSON_AddItemToArray(list, item)) {
                cJSON_Delete(item);
                return false;
            }
            if (cJSON_AddNumberToObject(item, "channel", (double)node->channel) == NULL ||
                cJSON_AddNumberToObject(item, "node_id", (double)node->node_id) == NULL ||
                cJSON_AddStringToObject(
                    item, "state", nrf_topology_leveling_node_state_name(node->state)) == NULL) {
                return false;
            }
            if (node->tilt_valid &&
                cJSON_AddNumberToObject(item, "tilt_deg", (double)node->tilt_deg) == NULL) {
                return false;
            }
        }
    }

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
        cJSON_AddBoolToObject(object, "bypassed", gate.bypassed) != NULL &&
        cJSON_AddBoolToObject(
            object,
            "allowed",
            storage_available && gate.start_allowed
        ) != NULL &&
        cJSON_AddStringToObject(
            object,
            "reason",
            storage_available
                ? recording_gate_decision_reason(gate.decision)
                : "STORAGE_UNAVAILABLE"
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
bool web_status_json_add_measurement(cJSON *root, bool *recording_out)
{
    if (recording_out != NULL) {
        *recording_out = false;
    }
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
    if (recording_out != NULL) {
        *recording_out = snapshot.state == MEASUREMENT_SERVICE_RECORDING;
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

static bool web_status_json_add_workflow_i16_array(
    cJSON *root,
    const char *name,
    const int16_t values[3],
    bool valid)
{
    cJSON *array = cJSON_AddArrayToObject(root, name);
    if (array == NULL) return false;
    for (size_t i = 0U; i < 3U; ++i) {
        cJSON *value = valid
            ? cJSON_CreateNumber((double)values[i])
            : cJSON_CreateNull();
        if (value == NULL || !cJSON_AddItemToArray(array, value)) {
            cJSON_Delete(value);
            return false;
        }
    }
    return true;
}

static bool web_status_json_add_workflow_calibration(
    cJSON *workflow,
    const workflow_calibration_snapshot_t *calibration)
{
    cJSON *object = cJSON_AddObjectToObject(workflow, "calibration");
    cJSON *nodes = object != NULL ? cJSON_AddArrayToObject(object, "per_node") : NULL;
    cJSON *cs = object != NULL ? cJSON_AddObjectToObject(object, "cs") : NULL;
    cJSON *reasons = object != NULL ? cJSON_AddArrayToObject(object, "reasons") : NULL;
    if (object == NULL || nodes == NULL || cs == NULL || reasons == NULL ||
        cJSON_AddNumberToObject(object, "generation", calibration->generation) == NULL ||
        cJSON_AddStringToObject(object, "phase", calibration->phase) == NULL ||
        cJSON_AddNumberToObject(cs, "pairs_done", (double)calibration->pairs_done) == NULL ||
        cJSON_AddNumberToObject(cs, "pairs_total", (double)calibration->pairs_total) == NULL) {
        return false;
    }
    if (calibration->max_spread_valid) {
        if (cJSON_AddNumberToObject(
                cs, "max_spread_mm", calibration->max_spread_mm) == NULL) {
            return false;
        }
    } else if (cJSON_AddNullToObject(cs, "max_spread_mm") == NULL) {
        return false;
    }
    if (calibration->result_valid) {
        if (cJSON_AddBoolToObject(object, "result", calibration->result_ok) == NULL) {
            return false;
        }
    } else if (cJSON_AddNullToObject(object, "result") == NULL) {
        return false;
    }

    const size_t node_count = calibration->per_node_count < WORKFLOW_MAX_CALIBRATION_NODES
        ? calibration->per_node_count
        : WORKFLOW_MAX_CALIBRATION_NODES;
    for (size_t i = 0U; i < node_count; ++i) {
        const workflow_calibration_node_t *source = &calibration->per_node[i];
        cJSON *node = cJSON_CreateObject();
        cJSON *height = node != NULL ? cJSON_AddObjectToObject(node, "height") : NULL;
        cJSON *imu = node != NULL ? cJSON_AddObjectToObject(node, "imu") : NULL;
        if (node == NULL || height == NULL || imu == NULL ||
            cJSON_AddStringToObject(node, "node", source->node) == NULL ||
            cJSON_AddBoolToObject(node, "cs_pairs_ok", source->cs_pairs_ok) == NULL ||
            cJSON_AddStringToObject(height, "state", source->height_state) == NULL ||
            cJSON_AddStringToObject(imu, "state", source->imu_state) == NULL) {
            cJSON_Delete(node);
            return false;
        }
        if (source->relative_z_mm_valid) {
            if (cJSON_AddNumberToObject(height, "relative_z_mm", source->relative_z_mm) == NULL) {
                cJSON_Delete(node);
                return false;
            }
        } else if (cJSON_AddNullToObject(height, "relative_z_mm") == NULL) {
            cJSON_Delete(node);
            return false;
        }
        if (!web_status_json_add_workflow_i16_array(
                imu, "gyro_bias_mdps", source->gyro_bias_mdps, source->imu_values_valid) ||
            !web_status_json_add_workflow_i16_array(
                imu, "gravity_ref_mg", source->gravity_ref_mg, source->imu_values_valid) ||
            !cJSON_AddItemToArray(nodes, node)) {
            cJSON_Delete(node);
            return false;
        }
    }
    const size_t reason_count = calibration->reasons_count < WORKFLOW_MAX_CALIBRATION_REASONS
        ? calibration->reasons_count
        : WORKFLOW_MAX_CALIBRATION_REASONS;
    for (size_t i = 0U; i < reason_count; ++i) {
        cJSON *reason = cJSON_CreateString(calibration->reasons[i]);
        if (reason == NULL || !cJSON_AddItemToArray(reasons, reason)) {
            cJSON_Delete(reason);
            return false;
        }
    }
    return true;
}

static bool web_status_json_add_workflow_placement(
    cJSON *workflow,
    const workflow_placement_snapshot_t *placement)
{
    cJSON *object = cJSON_AddObjectToObject(workflow, "placement");
    cJSON *nodes = object != NULL ? cJSON_AddArrayToObject(object, "per_node") : NULL;
    if (object == NULL || nodes == NULL ||
        cJSON_AddBoolToObject(object, "all_stable", placement->all_stable) == NULL) {
        return false;
    }
    const size_t node_count = placement->per_node_count < WORKFLOW_MAX_CALIBRATION_NODES
        ? placement->per_node_count
        : WORKFLOW_MAX_CALIBRATION_NODES;
    for (size_t i = 0U; i < node_count; ++i) {
        const workflow_placement_node_t *source = &placement->per_node[i];
        cJSON *node = cJSON_CreateObject();
        if (node == NULL ||
            cJSON_AddStringToObject(node, "node", source->node) == NULL ||
            cJSON_AddBoolToObject(node, "vertical", source->vertical) == NULL ||
            cJSON_AddBoolToObject(node, "stable", source->stable) == NULL) {
            cJSON_Delete(node);
            return false;
        }
        const bool height_added = source->height_rel_main_mm_valid
            ? cJSON_AddNumberToObject(
                node, "height_rel_main_mm", (double)source->height_rel_main_mm) != NULL
            : cJSON_AddNullToObject(node, "height_rel_main_mm") != NULL;
        const bool altitude_added = source->altitude_asl_m_valid
            ? cJSON_AddNumberToObject(node, "altitude_asl_m", source->altitude_asl_m) != NULL
            : cJSON_AddNullToObject(node, "altitude_asl_m") != NULL;
        if (!height_added || !altitude_added || !cJSON_AddItemToArray(nodes, node)) {
            cJSON_Delete(node);
            return false;
        }
    }
    return true;
}

bool web_status_json_add_workflow(cJSON *root)
{
    if (root == NULL) return false;
    workflow_snapshot_t snapshot;
    if (workflow_service_get_snapshot(&snapshot) != ESP_OK) return false;

    cJSON *workflow = cJSON_AddObjectToObject(root, "workflow");
    cJSON *actions = workflow != NULL
        ? cJSON_AddArrayToObject(workflow, "allowed_actions")
        : NULL;
    if (workflow == NULL || actions == NULL ||
        cJSON_AddStringToObject(workflow, "step", workflow_core_step_name(snapshot.step)) == NULL ||
        cJSON_AddNumberToObject(workflow, "generation", (double)snapshot.generation) == NULL ||
        cJSON_AddStringToObject(
            workflow,
            "sensor_sufficiency",
            workflow_core_sufficiency_name(snapshot.sensor_sufficiency)
        ) == NULL ||
        cJSON_AddNumberToObject(workflow, "assigned_remotes", snapshot.assigned_remotes) == NULL ||
        cJSON_AddStringToObject(workflow, "reason", snapshot.reason) == NULL ||
        cJSON_AddBoolToObject(workflow, "has_scan", snapshot.has_scan) == NULL ||
        cJSON_AddStringToObject(
            workflow, "gps_status", workflow_core_gps_status_name(snapshot.gps_status)) == NULL ||
        cJSON_AddStringToObject(
            workflow, "scan_choice", workflow_core_scan_choice_name(snapshot.scan_choice)) == NULL ||
        cJSON_AddStringToObject(
            workflow, "scan_upload_status",
            workflow_core_scan_upload_status_name(snapshot.scan_upload_status)) == NULL ||
        cJSON_AddBoolToObject(workflow, "calc_settings_ready", snapshot.calc_settings_ready) == NULL ||
        cJSON_AddStringToObject(
            workflow, "calculation_class",
            workflow_core_calculation_class_name(snapshot.calculation_class)) == NULL) {
        return false;
    }

    if (snapshot.layout_result_valid) {
        if (cJSON_AddBoolToObject(workflow, "layout_result", snapshot.layout_ok) == NULL) return false;
    } else if (cJSON_AddNullToObject(workflow, "layout_result") == NULL) {
        return false;
    }
    if (cJSON_AddStringToObject(workflow, "layout_reason", snapshot.reason) == NULL) return false;

    const bool object_id_added = snapshot.object_id[0] != '\0'
        ? cJSON_AddStringToObject(workflow, "object_id", snapshot.object_id) != NULL
        : cJSON_AddNullToObject(workflow, "object_id") != NULL;
    const bool session_id_added = snapshot.session_id[0] != '\0'
        ? cJSON_AddStringToObject(workflow, "session_id", snapshot.session_id) != NULL
        : cJSON_AddNullToObject(workflow, "session_id") != NULL;
    if (!object_id_added || !session_id_added) return false;

    if (snapshot.progress == WORKFLOW_PROGRESS_UNKNOWN) {
        if (cJSON_AddNullToObject(workflow, "progress") == NULL) return false;
    }
    else if (cJSON_AddNumberToObject(workflow, "progress", snapshot.progress) == NULL) {
        return false;
    }

    if (!web_status_json_add_workflow_calibration(workflow, &snapshot.calibration)) {
        return false;
    }
    if (!web_status_json_add_workflow_placement(workflow, &snapshot.placement)) {
        return false;
    }

    if (snapshot.recommended_action < WORKFLOW_COMMAND_COUNT &&
        (snapshot.allowed_actions & WORKFLOW_ACTION(snapshot.recommended_action)) != 0U) {
        cJSON *item = cJSON_CreateString(workflow_core_command_name(snapshot.recommended_action));
        if (item == NULL || !cJSON_AddItemToArray(actions, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    for (int i = 0; i < WORKFLOW_COMMAND_COUNT; ++i) {
        const workflow_command_t command = (workflow_command_t)i;
        if ((snapshot.allowed_actions & WORKFLOW_ACTION(command)) == 0U ||
            command == snapshot.recommended_action) {
            continue;
        }
        cJSON *item = cJSON_CreateString(workflow_core_command_name(command));
        if (item == NULL || !cJSON_AddItemToArray(actions, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    return true;
}
