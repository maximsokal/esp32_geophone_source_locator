#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "seismic_recorder.h"
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


int main(void)
{
    test_planar_gate_is_serialized_with_warning();
    test_not_level_gate_is_serialized_as_blocked();
    test_three_dimensional_gate_is_serialized_without_warning();
    test_insufficient_gate_is_serialized_as_blocked();
    test_topology_error_is_available_false_with_error_name();
    test_null_root_is_rejected_without_topology_access();
    puts("web status recording gate host tests passed");
    return 0;
}
