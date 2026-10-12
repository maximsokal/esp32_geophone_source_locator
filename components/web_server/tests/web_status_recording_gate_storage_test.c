#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "seismic_recorder.h"
#include "storage_manager.h"
#include "web_server_internal.h"
#include "workflow_service.h"

bool web_status_json_add_workflow(cJSON *root);

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "ESP_ERR";
}

static nrf_topology_leveling_summary_t s_leveling;
static esp_err_t s_leveling_error;
static workflow_snapshot_t s_workflow_snapshot;

esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary
)
{
    if (s_leveling_error != ESP_OK) return s_leveling_error;
    *summary = s_leveling;
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes)
{
    (void)nodes;
    return ESP_ERR_INVALID_STATE;
}

const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state)
{
    (void)state;
    return "no_data";
}

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}

const char *seismic_recorder_stop_stage_name(seismic_recorder_stop_stage_t stage)
{
    (void)stage;
    return "NONE";
}

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    *snapshot = s_workflow_snapshot;
    return ESP_OK;
}

const char *workflow_core_step_name(workflow_step_t step)
{
    (void)step;
    return "UNKNOWN";
}

const char *workflow_core_sufficiency_name(workflow_sensor_sufficiency_t sufficiency)
{
    (void)sufficiency;
    return "INSUFFICIENT";
}

const char *workflow_core_command_name(workflow_command_t command)
{
    (void)command;
    return "UNKNOWN";
}

const char *workflow_core_calculation_class_name(
    workflow_calculation_class_t calculation_class
)
{
    (void)calculation_class;
    return "none";
}

const char *workflow_core_gps_status_name(workflow_gps_status_t status)
{
    switch (status) {
        case WORKFLOW_GPS_STATUS_SKIPPED: return "skipped";
        case WORKFLOW_GPS_STATUS_PENDING: return "pending";
        case WORKFLOW_GPS_STATUS_ACCEPTED: return "accepted";
        case WORKFLOW_GPS_STATUS_FAILED: return "failed";
        case WORKFLOW_GPS_STATUS_NONE:
        default: return "none";
    }
}

const char *workflow_core_scan_choice_name(workflow_scan_choice_t choice)
{
    switch (choice) {
        case WORKFLOW_SCAN_CHOICE_YES: return "yes";
        case WORKFLOW_SCAN_CHOICE_NO: return "no";
        case WORKFLOW_SCAN_CHOICE_NONE:
        default: return "none";
    }
}

const char *workflow_core_scan_upload_status_name(workflow_scan_upload_status_t status)
{
    switch (status) {
        case WORKFLOW_SCAN_UPLOAD_PENDING: return "pending";
        case WORKFLOW_SCAN_UPLOAD_VERIFIED: return "verified";
        case WORKFLOW_SCAN_UPLOAD_FAILED: return "failed";
        case WORKFLOW_SCAN_UPLOAD_NONE:
        default: return "none";
    }
}

static const cJSON *field(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(value != NULL);
    return value;
}

static void assert_string(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsString(value));
    assert(strcmp(value->valuestring, expected) == 0);
}

static void assert_bool(const cJSON *object, const char *name, bool expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsBool(value));
    assert(cJSON_IsTrue(value) == expected);
}

static void test_storage_unavailable_blocks_an_eligible_topology(void)
{
    s_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 4U, .accepted_count = 4U };
    s_leveling_error = ESP_OK;
    storage_manager_test_set_ready(false);
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = field(root, "recording_gate");
    assert_bool(gate, "available", true);
    assert_bool(gate, "allowed", false);
    assert_string(gate, "decision", "ALLOWED_3D_CAPABLE");
    assert_string(gate, "reason", "STORAGE_UNAVAILABLE");
    cJSON_Delete(root);
}

static void test_storage_unavailable_reason_survives_topology_error(void)
{
    s_leveling_error = ESP_ERR_INVALID_STATE;
    storage_manager_test_set_ready(false);
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = field(root, "recording_gate");
    assert_bool(gate, "available", false);
    assert_bool(gate, "allowed", false);
    assert_string(gate, "reason", "STORAGE_UNAVAILABLE");
    cJSON_Delete(root);
}

static void test_available_storage_preserves_topology_gate(void)
{
    s_leveling = (nrf_topology_leveling_summary_t){ .assigned_count = 4U, .accepted_count = 4U };
    s_leveling_error = ESP_OK;
    storage_manager_test_set_ready(true);
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);

    assert(web_status_json_add_recording_gate(root));
    const cJSON *gate = field(root, "recording_gate");
    assert_bool(gate, "available", true);
    assert_bool(gate, "allowed", true);
    assert_string(gate, "reason", "");
    cJSON_Delete(root);
}

static void test_workflow_json_serializes_gps_layout_and_scan_status(void)
{
    s_workflow_snapshot.step = WORKFLOW_STEP_SCANNING;
    s_workflow_snapshot.gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
    s_workflow_snapshot.layout_result_valid = true;
    s_workflow_snapshot.layout_ok = true;
    s_workflow_snapshot.scan_choice = WORKFLOW_SCAN_CHOICE_YES;
    s_workflow_snapshot.scan_upload_status = WORKFLOW_SCAN_UPLOAD_PENDING;
    s_workflow_snapshot.progress = WORKFLOW_PROGRESS_UNKNOWN;
    (void)snprintf(s_workflow_snapshot.reason, sizeof(s_workflow_snapshot.reason), "%s", "");

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_workflow(root));
    const cJSON *workflow = field(root, "workflow");
    assert_string(workflow, "gps_status", "accepted");
    assert_bool(workflow, "layout_result", true);
    assert_string(workflow, "scan_choice", "yes");
    assert_string(workflow, "scan_upload_status", "pending");
    cJSON_Delete(root);
}

int main(void)
{
    test_storage_unavailable_blocks_an_eligible_topology();
    test_storage_unavailable_reason_survives_topology_error();
    test_available_storage_preserves_topology_gate();
    test_workflow_json_serializes_gps_layout_and_scan_status();
    puts("web_status_recording_gate_storage_test: PASS");
    return 0;
}
