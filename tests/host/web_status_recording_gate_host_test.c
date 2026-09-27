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
    puts("web status recording gate host tests passed");
    return 0;
}
