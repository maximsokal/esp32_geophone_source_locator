#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "seismic_recorder.h"
#include "workflow_service.h"
#include "web_server_internal.h"


static nrf_topology_leveling_summary_t g_leveling;
static esp_err_t g_leveling_error;


esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary
)
{
    if (g_leveling_error != ESP_OK) {
        return g_leveling_error;
    }
    *summary = g_leveling;
    return ESP_OK;
}


static nrf_topology_leveling_nodes_t g_nodes;
static esp_err_t g_nodes_error = ESP_OK;
static workflow_snapshot_t g_workflow_snapshot;
static esp_err_t g_workflow_snapshot_error = ESP_OK;
static unsigned g_workflow_snapshot_calls;

bool web_status_json_add_workflow(cJSON *root);

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    ++g_workflow_snapshot_calls;
    if (g_workflow_snapshot_error != ESP_OK) return g_workflow_snapshot_error;
    *snapshot = g_workflow_snapshot;
    return ESP_OK;
}

const char *workflow_core_step_name(workflow_step_t step)
{
    return step == WORKFLOW_STEP_RESULT ? "RESULT" : "OTHER";
}

const char *workflow_core_sufficiency_name(workflow_sensor_sufficiency_t sufficiency)
{
    return sufficiency == WORKFLOW_SENSOR_CAPABLE_3D ? "CAPABLE_3D" : "INSUFFICIENT";
}

const char *workflow_core_command_name(workflow_command_t command)
{
    switch (command) {
        case WORKFLOW_COMMAND_MOVE_SENSORS: return "MOVE_SENSORS";
        case WORKFLOW_COMMAND_RECORD_AGAIN: return "RECORD_AGAIN";
        case WORKFLOW_COMMAND_BACK: return "BACK";
        default: return "OTHER";
    }
}

esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes)
{
    if (g_nodes_error != ESP_OK) {
        return g_nodes_error;
    }
    *nodes = g_nodes;
    return ESP_OK;
}

const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state)
{
    return state == NRF_TOPOLOGY_LEVELING_ACCEPTED ? "level"
        : state == NRF_TOPOLOGY_LEVELING_OUTSIDE ? "tilted" : "no_data";
}


esp_err_t measurement_service_get_snapshot(
    measurement_service_snapshot_t *snapshot
)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}


esp_err_t seismic_recorder_get_snapshot(
    seismic_recorder_snapshot_t *snapshot
)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}


const char *seismic_recorder_stop_stage_name(
    seismic_recorder_stop_stage_t stage
)
{
    (void)stage;
    return "NONE";
}


const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_ERR_INVALID_STATE:
            return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND:
            return "ESP_ERR_NOT_FOUND";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}


static const cJSON *item(const cJSON *root, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    assert(value != NULL);
    return value;
}


static void assert_string(const cJSON *root, const char *name, const char *expected)
{
    const cJSON *value = item(root, name);
    assert(cJSON_IsString(value));
    assert(strcmp(value->valuestring, expected) == 0);
}


static void assert_number(const cJSON *root, const char *name, double expected)
{
    const cJSON *value = item(root, name);
    assert(cJSON_IsNumber(value));
    assert(value->valuedouble == expected);
}


static void assert_bool(const cJSON *root, const char *name, bool expected)
{
    const cJSON *value = item(root, name);
    assert(cJSON_IsBool(value));
    assert(cJSON_IsTrue(value) == expected);
}


static void test_planar_gate_is_serialized_with_warning(void)
{
    g_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 3U, .accepted_count = 3U };
    g_leveling_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = item(root, "recording_gate");
    assert_bool(gate, "available", true);
    assert_number(gate, "assigned_count", 3.0);
    assert_number(gate, "accepted_count", 3.0);
    assert_string(gate, "decision", "ALLOWED_PLANAR_2D");
    assert_string(gate, "dimensionality", "PLANAR_2D");
    assert_bool(gate, "allowed", true);
    assert_string(gate, "reason", "");
    assert(strstr(item(gate, "warning")->valuestring, "planar/2D") != NULL);
    cJSON_Delete(root);
}


static void test_not_level_gate_is_serialized_as_blocked(void)
{
    g_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 4U, .accepted_count = 3U };
    g_leveling_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = item(root, "recording_gate");
    assert_string(gate, "decision", "BLOCKED_NOT_LEVEL");
    assert_string(gate, "dimensionality", "CAPABLE_3D");
    assert_bool(gate, "allowed", false);
    assert(strstr(item(gate, "reason")->valuestring, "not reported") != NULL);
    assert_string(gate, "warning", "");
    cJSON_Delete(root);
}


/* MAX-114: the per-node list tells the operator which node is not level. */
static void test_per_node_leveling_is_serialized(void)
{
    g_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 2U, .accepted_count = 1U };
    g_leveling_error = ESP_OK;
    g_nodes_error = ESP_OK;
    g_nodes.count = 2U;
    g_nodes.nodes[0] = (nrf_topology_leveling_node_t){ .channel = 0U, .node_id = 1U,
        .state = NRF_TOPOLOGY_LEVELING_ACCEPTED, .tilt_valid = true, .tilt_deg = 0.5f };
    g_nodes.nodes[1] = (nrf_topology_leveling_node_t){ .channel = 3U, .node_id = 4U,
        .state = NRF_TOPOLOGY_LEVELING_NO_DATA, .tilt_valid = false, .tilt_deg = 0.0f };
    cJSON *root = cJSON_CreateObject();
    assert(web_status_json_add_recording_gate(root));
    const cJSON *nodes = item(item(root, "recording_gate"), "nodes");
    assert(cJSON_GetArraySize(nodes) == 2);
    const cJSON *first = cJSON_GetArrayItem(nodes, 0);
    assert_string(first, "state", "level");
    assert(item(first, "tilt_deg")->valuedouble > 0.49 && item(first, "tilt_deg")->valuedouble < 0.51);
    const cJSON *second = cJSON_GetArrayItem(nodes, 1);
    assert(item(second, "channel")->valuedouble == 3.0);
    assert_string(second, "state", "no_data");
    /* No tilt without a reading. */
    assert(cJSON_GetObjectItemCaseSensitive(second, "tilt_deg") == NULL);
    cJSON_Delete(root);

    /* A failing per-node read never breaks the gate itself. */
    g_nodes_error = ESP_ERR_INVALID_STATE;
    root = cJSON_CreateObject();
    assert(web_status_json_add_recording_gate(root));
    assert(cJSON_GetObjectItemCaseSensitive(item(root, "recording_gate"), "nodes") == NULL);
    cJSON_Delete(root);
    g_nodes_error = ESP_OK;
    g_nodes.count = 0U;
}


static void test_three_dimensional_gate_is_serialized_without_warning(void)
{
    g_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 4U, .accepted_count = 4U };
    g_leveling_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = item(root, "recording_gate");
    assert_string(gate, "decision", "ALLOWED_3D_CAPABLE");
    assert_string(gate, "dimensionality", "CAPABLE_3D");
    assert_bool(gate, "allowed", true);
    assert_string(gate, "reason", "");
    assert_string(gate, "warning", "");
    cJSON_Delete(root);
}


static void test_insufficient_gate_is_serialized_as_blocked(void)
{
    g_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 2U, .accepted_count = 2U };
    g_leveling_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = item(root, "recording_gate");
    assert_string(gate, "decision", "BLOCKED_INSUFFICIENT_REMOTES");
    assert_string(gate, "dimensionality", "INSUFFICIENT");
    assert_bool(gate, "allowed", false);
    assert(strstr(item(gate, "reason")->valuestring, "fewer than 3") != NULL);
    assert_string(gate, "warning", "");
    cJSON_Delete(root);
}


static void test_topology_error_is_available_false_with_error_name(void)
{
    g_leveling_error = ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = item(root, "recording_gate");
    assert_bool(gate, "available", false);
    assert_string(gate, "error", "ESP_ERR_INVALID_STATE");
    assert(cJSON_GetObjectItemCaseSensitive(gate, "decision") == NULL);
    cJSON_Delete(root);
}


static void test_null_root_is_rejected_without_topology_access(void)
{
    g_leveling_error = ESP_FAIL;
    assert(!web_status_json_add_recording_gate(NULL));
}

static void test_workflow_json_orders_recommended_action_and_handles_unknown_progress(void)
{
    g_workflow_snapshot = (workflow_snapshot_t){
        .step = WORKFLOW_STEP_RESULT,
        .generation = 27U,
        .allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS) |
            WORKFLOW_ACTION(WORKFLOW_COMMAND_RECORD_AGAIN) |
            WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK),
        .sensor_sufficiency = WORKFLOW_SENSOR_CAPABLE_3D,
        .assigned_remotes = 4U,
        .reason = "geometry_degenerate",
        .progress = WORKFLOW_PROGRESS_UNKNOWN,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS,
        .recommended_action = WORKFLOW_COMMAND_MOVE_SENSORS,
    };
    g_workflow_snapshot_error = ESP_OK;
    g_workflow_snapshot_calls = 0U;
    cJSON *root = cJSON_CreateObject();
    assert(web_status_json_add_workflow(root));

    const cJSON *workflow = item(root, "workflow");
    assert_string(workflow, "step", "RESULT");
    assert_number(workflow, "generation", 27.0);
    assert_string(workflow, "sensor_sufficiency", "CAPABLE_3D");
    assert_number(workflow, "assigned_remotes", 4.0);
    assert_string(workflow, "reason", "geometry_degenerate");
    assert(cJSON_IsNull(item(workflow, "progress")));
    const cJSON *actions = item(workflow, "allowed_actions");
    assert(cJSON_GetArraySize(actions) == 3);
    assert(cJSON_IsString(cJSON_GetArrayItem(actions, 0)));
    assert(strcmp(cJSON_GetArrayItem(actions, 0)->valuestring, "MOVE_SENSORS") == 0);
    assert(strcmp(cJSON_GetArrayItem(actions, 1)->valuestring, "RECORD_AGAIN") == 0);
    assert(strcmp(cJSON_GetArrayItem(actions, 2)->valuestring, "BACK") == 0);
    assert(g_workflow_snapshot_calls == 1U);
    cJSON_Delete(root);

    g_workflow_snapshot.progress = 64;
    g_workflow_snapshot.recommended_action = WORKFLOW_COMMAND_COUNT;
    root = cJSON_CreateObject();
    assert(web_status_json_add_workflow(root));
    workflow = item(root, "workflow");
    assert_number(workflow, "progress", 64.0);
    cJSON_Delete(root);

    g_workflow_snapshot_error = ESP_ERR_INVALID_STATE;
    root = cJSON_CreateObject();
    assert(!web_status_json_add_workflow(root));
    assert(cJSON_GetObjectItemCaseSensitive(root, "workflow") == NULL);
    cJSON_Delete(root);
    const unsigned calls_before_null = g_workflow_snapshot_calls;
    assert(!web_status_json_add_workflow(NULL));
    assert(g_workflow_snapshot_calls == calls_before_null);
}

static void test_workflow_calibration_json_serializes_values_and_nulls(void)
{
    g_workflow_snapshot = (workflow_snapshot_t){
        .step = WORKFLOW_STEP_CALIBRATING,
        .generation = 28U,
        .progress = WORKFLOW_PROGRESS_UNKNOWN,
        .calibration = {
            .generation = 91U,
            .phase = "running",
            .per_node_count = 1U,
            .per_node = {{
                .node = "MAIN",
                .height_state = "pending",
                .imu_state = "not_applicable",
                .cs_pairs_ok = false,
            }},
            .pairs_done = 0U,
            .pairs_total = 3U,
        },
        .placement = {
            .per_node_count = 2U,
            .per_node = {
                {
                    .node = "MAIN",
                    .vertical = true,
                    .height_rel_main_mm_valid = false,
                    .altitude_asl_m_valid = false,
                    .stable = false,
                },
                {
                    .node = "G1",
                    .vertical = false,
                    .height_rel_main_mm_valid = false,
                    .altitude_asl_m_valid = false,
                    .stable = false,
                },
            },
            .all_stable = false,
        },
    };
    g_workflow_snapshot_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();
    assert(web_status_json_add_workflow(root));
    const cJSON *calibration = item(item(root, "workflow"), "calibration");
    assert_number(calibration, "generation", 91.0);
    assert_string(calibration, "phase", "running");
    assert(cJSON_IsNull(item(calibration, "result")));
    assert(cJSON_GetArraySize(item(calibration, "reasons")) == 0);
    const cJSON *cs = item(calibration, "cs");
    assert_number(cs, "pairs_done", 0.0);
    assert_number(cs, "pairs_total", 3.0);
    assert(cJSON_IsNull(item(cs, "max_spread_mm")));
    const cJSON *nodes = item(calibration, "per_node");
    assert(cJSON_GetArraySize(nodes) == 1);
    const cJSON *node = cJSON_GetArrayItem(nodes, 0);
    assert_string(node, "node", "MAIN");
    assert_bool(node, "cs_pairs_ok", false);
    const cJSON *height = item(node, "height");
    assert_string(height, "state", "pending");
    assert(cJSON_IsNull(item(height, "relative_z_mm")));
    const cJSON *imu = item(node, "imu");
    assert_string(imu, "state", "not_applicable");
    const cJSON *gyro = item(imu, "gyro_bias_mdps");
    const cJSON *gravity = item(imu, "gravity_ref_mg");
    assert(cJSON_GetArraySize(gyro) == 3 && cJSON_GetArraySize(gravity) == 3);
    for (int i = 0; i < 3; ++i) {
        assert(cJSON_IsNull(cJSON_GetArrayItem(gyro, i)));
        assert(cJSON_IsNull(cJSON_GetArrayItem(gravity, i)));
    }
    const cJSON *placement = item(item(root, "workflow"), "placement");
    assert_bool(placement, "all_stable", false);
    const cJSON *placement_nodes = item(placement, "per_node");
    assert(cJSON_GetArraySize(placement_nodes) == 2);
    const cJSON *placement_node = cJSON_GetArrayItem(placement_nodes, 0);
    assert_string(placement_node, "node", "MAIN");
    assert_bool(placement_node, "vertical", true);
    assert(cJSON_IsNull(item(placement_node, "height_rel_main_mm")));
    assert(cJSON_IsNull(item(placement_node, "altitude_asl_m")));
    assert_bool(placement_node, "stable", false);
    placement_node = cJSON_GetArrayItem(placement_nodes, 1);
    assert_string(placement_node, "node", "G1");
    assert_bool(placement_node, "vertical", false);
    assert(cJSON_IsNull(item(placement_node, "height_rel_main_mm")));
    assert(cJSON_IsNull(item(placement_node, "altitude_asl_m")));
    assert_bool(placement_node, "stable", false);
    cJSON_Delete(root);

    workflow_calibration_node_t *source =
        &g_workflow_snapshot.calibration.per_node[0];
    (void)snprintf(source->height_state, sizeof(source->height_state), "%s", "valid");
    source->relative_z_mm_valid = true;
    source->relative_z_mm = -23;
    (void)snprintf(source->imu_state, sizeof(source->imu_state), "%s", "calibrated");
    source->imu_values_valid = true;
    source->gyro_bias_mdps[0] = -11;
    source->gyro_bias_mdps[1] = 22;
    source->gyro_bias_mdps[2] = -33;
    source->gravity_ref_mg[0] = 1;
    source->gravity_ref_mg[1] = 2;
    source->gravity_ref_mg[2] = 997;
    source->cs_pairs_ok = true;
    g_workflow_snapshot.placement.all_stable = true;
    g_workflow_snapshot.placement.per_node[0] = (workflow_placement_node_t){
        .node = "MAIN",
        .vertical = true,
        .height_rel_main_mm_valid = true,
        .height_rel_main_mm = 0,
        .altitude_asl_m_valid = true,
        .altitude_asl_m = 0.0,
        .stable = true,
    };
    g_workflow_snapshot.placement.per_node[1] = (workflow_placement_node_t){
        .node = "G1",
        .vertical = true,
        .height_rel_main_mm_valid = true,
        .height_rel_main_mm = -23,
        .altitude_asl_m_valid = true,
        .altitude_asl_m = 12.345,
        .stable = true,
    };
    g_workflow_snapshot.calibration.max_spread_valid = true;
    g_workflow_snapshot.calibration.max_spread_mm = 149;
    g_workflow_snapshot.calibration.result_valid = true;
    g_workflow_snapshot.calibration.result_ok = false;
    g_workflow_snapshot.calibration.reasons_count = 1U;
    (void)snprintf(g_workflow_snapshot.calibration.reasons[0],
        WORKFLOW_CALIBRATION_REASON_MAX, "%s", "G1:height_rejected_moving");
    root = cJSON_CreateObject();
    assert(web_status_json_add_workflow(root));
    calibration = item(item(root, "workflow"), "calibration");
    assert(cJSON_IsBool(item(calibration, "result")) &&
        !cJSON_IsTrue(item(calibration, "result")));
    const cJSON *reason = cJSON_GetArrayItem(item(calibration, "reasons"), 0);
    assert(cJSON_IsString(reason));
    assert(strcmp(reason->valuestring, "G1:height_rejected_moving") == 0);
    assert_number(item(calibration, "cs"), "max_spread_mm", 149.0);
    node = cJSON_GetArrayItem(item(calibration, "per_node"), 0);
    assert_bool(node, "cs_pairs_ok", true);
    assert(item(item(node, "height"), "relative_z_mm")->valuedouble == -23.0);
    assert(cJSON_GetArrayItem(item(item(node, "imu"), "gyro_bias_mdps"), 0)->valuedouble == -11.0);
    assert(cJSON_GetArrayItem(item(item(node, "imu"), "gravity_ref_mg"), 2)->valuedouble == 997.0);
    placement = item(item(root, "workflow"), "placement");
    assert_bool(placement, "all_stable", true);
    placement_nodes = item(placement, "per_node");
    placement_node = cJSON_GetArrayItem(placement_nodes, 0);
    assert_string(placement_node, "node", "MAIN");
    assert_bool(placement_node, "vertical", true);
    assert(item(placement_node, "height_rel_main_mm")->valuedouble == 0.0);
    assert(item(placement_node, "altitude_asl_m")->valuedouble == 0.0);
    assert_bool(placement_node, "stable", true);
    placement_node = cJSON_GetArrayItem(placement_nodes, 1);
    assert_string(placement_node, "node", "G1");
    assert_bool(placement_node, "vertical", true);
    assert(item(placement_node, "height_rel_main_mm")->valuedouble == -23.0);
    assert(item(placement_node, "altitude_asl_m")->valuedouble == 12.345);
    assert_bool(placement_node, "stable", true);
    cJSON_Delete(root);
}


int main(void)
{
    test_planar_gate_is_serialized_with_warning();
    test_not_level_gate_is_serialized_as_blocked();
    test_three_dimensional_gate_is_serialized_without_warning();
    test_insufficient_gate_is_serialized_as_blocked();
    test_topology_error_is_available_false_with_error_name();
    test_null_root_is_rejected_without_topology_access();
    test_per_node_leveling_is_serialized();
    test_workflow_json_orders_recommended_action_and_handles_unknown_progress();
    test_workflow_calibration_json_serializes_values_and_nulls();
    puts("web status recording gate host tests passed");
    return 0;
}
