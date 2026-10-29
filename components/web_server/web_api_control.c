#include "web_server_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_number.h"
#include "object_store.h"
#include "storage_bulk_admission.h"
#include "measurement_service.h"
#include "seismic_acquisition.h"
#include "localization_calculate.h"
#include "localization_config.h"
#include "localization_job.h"
#include "localization_material_presets.h"
#include "main_barometer.h"
#include "nrf_topology_service.h"
#include "workflow_service.h"
#include "esp_log.h"
#include "esp_timer.h"

esp_err_t web_scan_store_clear_staging(const char *object_id);

#define WEB_CONTROL_MAX_BODY 1024U
#define WEB_CONTROL_ACTION_SIZE 48U
#define WEB_CONTROL_SCAN_QUERY_SIZE 160U
#define WEB_CONTROL_SCAN_ACTION_SIZE 16U

static const char *TAG = "WEB_API_CONTROL";

bool web_status_json_add_workflow(cJSON *root);
static const char *web_control_string(cJSON *root, const char *key);

static bool web_control_requires_object_cache_refresh(const char *action)
{
    if (action == NULL) return false;
    return
        strcmp(action, "create_object") == 0 ||
        strcmp(action, "delete_object") == 0 ||
        strcmp(action, "rename_object") == 0 ||
        strcmp(action, "set_object_cable_length") == 0 ||
        strcmp(action, "set_default_cable_length") == 0 ||
        strcmp(action, "set_object_location") == 0;
}


static bool web_control_is_object_storage_mutation(const char *action)
{
    return
        web_control_requires_object_cache_refresh(action) ||
        (action != NULL && (strcmp(action, "activate_object") == 0 ||
            strcmp(action, "save_calc_settings") == 0));
}


static bool web_control_object_storage_is_idle(void)
{
    measurement_service_snapshot_t measurement;
    if (measurement_service_get_snapshot(&measurement) != ESP_OK) {
        return false;
    }

    return measurement_service_snapshot_allows_object_mutation(&measurement);
}

static esp_err_t web_control_read_body(
    httpd_req_t *request,
    char *buffer,
    size_t buffer_size
)
{
    if (
        request == NULL ||
        buffer == NULL ||
        buffer_size < 2U ||
        request->content_len <= 0 ||
        request->content_len >= (int)buffer_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    while (offset < (size_t)request->content_len) {
        if (web_server_shutdown_requested()) {
            return ESP_ERR_INVALID_STATE;
        }

        const int received = httpd_req_recv(
            request,
            buffer + offset,
            (size_t)request->content_len - offset
        );
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (web_server_small_body_timeout_retry_allowed()) {
                continue;
            }
            return ESP_ERR_TIMEOUT;
        }
        if (received <= 0) return ESP_FAIL;

        offset += (size_t)received;

        /* A latch may become authoritative while recv() is blocked. Check
         * after every successful chunk too, including the final body chunk. */
        if (web_server_shutdown_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    buffer[offset] = '\0';
    return ESP_OK;
}

static esp_err_t web_control_send_result_with_error_class(
    httpd_req_t *request,
    const char *action,
    esp_err_t result,
    const char *object_id,
    const char *error_class
)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    /* Every field is part of the response contract: a failed add (out of
     * memory) must not turn into a "successful" reply that lacks ok/action/
     * error, so nothing is sent unless the object is complete. */
    const bool workflow_step_error =
        error_class != NULL &&
        strcmp(error_class, "workflow_step_not_allowed") == 0;
    const bool complete =
        cJSON_AddBoolToObject(root, "ok", result == ESP_OK) != NULL &&
        cJSON_AddStringToObject(root, "action", action == NULL ? "UNKNOWN" : action) != NULL &&
        (object_id == NULL ||
         cJSON_AddStringToObject(root, "object_id", object_id) != NULL) &&
        (result == ESP_OK ||
         cJSON_AddStringToObject(root, "error", esp_err_to_name(result)) != NULL) &&
        (error_class == NULL ||
         cJSON_AddStringToObject(root, "error_class", error_class) != NULL) &&
        (!workflow_step_error ||
         (cJSON_AddStringToObject(root, "error_code", "workflow_step_not_allowed") != NULL &&
          cJSON_AddStringToObject(
              root,
              "message",
              "measurement_start is only allowed in RECORD_READY or RECORDING"
          ) != NULL));
    if (!complete) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (error_class != NULL && strcmp(error_class, "workflow_step_not_allowed") != 0) {
        httpd_resp_set_status(request, "503 Service Unavailable");
    }
    else if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE || result == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_status(request, "400 Bad Request");
    }
    else if (result == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(request, "404 Not Found");
    }
    else if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_TIMEOUT) {
        httpd_resp_set_status(request, "409 Conflict");
    }
    else if (result != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
    }

    const esp_err_t send_result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_result;
}

static esp_err_t web_control_send_result(
    httpd_req_t *request,
    const char *action,
    esp_err_t result,
    const char *object_id
)
{
    return web_control_send_result_with_error_class(request, action, result, object_id, NULL);
}

static esp_err_t web_control_send_height_calibration_result(
    httpd_req_t *request,
    uint8_t channel_mask,
    const char *error,
    const char *busy_reason)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    if (cJSON_AddBoolToObject(root, "ok", error == NULL && busy_reason == NULL) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (busy_reason != NULL) {
        if (cJSON_AddStringToObject(root, "busy_reason", busy_reason) == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    else if (error != NULL) {
        if (cJSON_AddStringToObject(root, "error", error) == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    else {
        if (cJSON_AddStringToObject(root, "action", "calibrate_heights") == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON *channels = cJSON_AddArrayToObject(root, "requested_channels");
        if (channels == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if ((channel_mask & (uint8_t)(1U << channel)) != 0U) {
                cJSON *item = cJSON_CreateNumber((double)channel);
                if (item == NULL || !cJSON_AddItemToArray(channels, item)) {
                    cJSON_Delete(item);
                    cJSON_Delete(root);
                    return ESP_ERR_NO_MEM;
                }
            }
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (error != NULL || busy_reason != NULL) {
        httpd_resp_set_status(request, "409 Conflict");
    }
    const esp_err_t send_result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_result;
}

static esp_err_t web_control_send_workflow_result(
    httpd_req_t *request,
    workflow_command_result_t result
)
{
    if (result == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT) {
        return web_control_send_result(
            request,
            "workflow_command",
            ESP_ERR_INVALID_ARG,
            NULL
        );
    }
    if (result == WORKFLOW_COMMAND_RESULT_UNAVAILABLE) {
        return web_control_send_result(
            request,
            "workflow_command",
            ESP_ERR_INVALID_STATE,
            NULL
        );
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    const bool accepted = result == WORKFLOW_COMMAND_RESULT_OK;
    if (cJSON_AddBoolToObject(root, "ok", accepted) == NULL ||
        (accepted
            ? !web_status_json_add_workflow(root)
            : cJSON_AddStringToObject(
                root,
                "reason",
                result == WORKFLOW_COMMAND_RESULT_STALE ? "stale" : "not_allowed"
            ) == NULL)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!accepted) httpd_resp_set_status(request, "409 Conflict");
    const esp_err_t send_result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_result;
}

static esp_err_t web_control_send_internal_workflow_command(httpd_req_t *request)
{
    static const char response[] = "{\"ok\":false,\"reason\":\"internal_command\"}";
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static bool web_control_number(cJSON *root, const char *key, double *value);

static workflow_command_result_t web_control_workflow_command(
    cJSON *root,
    bool *internal_command
)
{
    *internal_command = false;
    workflow_command_t command;
    const char *command_name = web_control_string(root, "command");
    if (!workflow_core_command_from_name(command_name, &command)) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }
    if (command == WORKFLOW_COMMAND_CALIBRATION_DONE ||
        command == WORKFLOW_COMMAND_RANGING_DONE ||
        command == WORKFLOW_COMMAND_CALCULATION_DONE) {
        *internal_command = true;
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }

    workflow_command_params_t params = {
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    if (generation != NULL) {
        const double value = generation->valuedouble;
        if (!cJSON_IsNumber(generation) || !isfinite(value) || value < 0.0 ||
            value > UINT32_MAX || floor(value) != value) {
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        }
        params.has_generation = true;
        params.expected_generation = (uint32_t)value;
    }
    if (!params.has_generation) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }

    if (command == WORKFLOW_COMMAND_SET_DISTANCE) {
        double a, b, distance, started, updated;
        if (!web_control_number(root, "node_a", &a) || !web_control_number(root, "node_b", &b) ||
            !web_control_number(root, "distance_mm", &distance) ||
            !web_control_number(root, "ranging_started_at_ms", &started) ||
            !web_control_number(root, "ranging_updated_at_ms", &updated) ||
            a < 0 || b < 0 || a > NRF_TOPOLOGY_REMOTE_COUNT || b > NRF_TOPOLOGY_REMOTE_COUNT ||
            floor(a) != a || floor(b) != b || a == b || distance <= 0 ||
            distance > INT32_MAX || floor(distance) != distance ||
            started < 0 || updated < 0 || started > 9007199254740991.0 || updated > 9007199254740991.0 ||
            floor(started) != started || floor(updated) != updated)
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        params.has_distance = true;
        params.node_a = (uint8_t)a; params.node_b = (uint8_t)b;
        params.distance_mm = (int32_t)distance;
        params.ranging_started_at_ms = (uint64_t)started;
        params.ranging_updated_at_ms = (uint64_t)updated;
    }

    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (ok != NULL) {
        if (!cJSON_IsBool(ok)) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        params.has_ok = true;
        params.ok = cJSON_IsTrue(ok);
    }

    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (reason != NULL) {
        if (!cJSON_IsString(reason) || reason->valuestring == NULL ||
            strnlen(reason->valuestring, WORKFLOW_REASON_MAX) >= WORKFLOW_REASON_MAX) {
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        }
        params.reason = reason->valuestring;
    }

    const cJSON *calculation_class = cJSON_GetObjectItemCaseSensitive(root, "class");
    if (calculation_class != NULL &&
        (!cJSON_IsString(calculation_class) ||
         !workflow_core_calculation_class_from_name(
             calculation_class->valuestring,
             &params.calculation_class
         ))) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }
    if (command == WORKFLOW_COMMAND_GPS_ACCEPT) {
        double latitude = 0.0;
        double longitude = 0.0;
        double accuracy = 0.0;
        double captured_at_ms = 0.0;
        double altitude = 0.0;
        double altitude_accuracy = 0.0;
        const cJSON *altitude_value = cJSON_GetObjectItemCaseSensitive(root, "altitude_m");
        const cJSON *altitude_accuracy_value =
            cJSON_GetObjectItemCaseSensitive(root, "altitude_accuracy_m");
        if (!web_control_number(root, "latitude", &latitude) ||
            !web_control_number(root, "longitude", &longitude) ||
            !web_control_number(root, "accuracy_m", &accuracy) ||
            !web_control_number(root, "captured_at_ms", &captured_at_ms) ||
            captured_at_ms < 0.0 || captured_at_ms > 9007199254740991.0 ||
            floor(captured_at_ms) != captured_at_ms) {
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        }
        if (altitude_value != NULL &&
            (!web_control_number(root, "altitude_m", &altitude) ||
             (altitude_accuracy_value != NULL &&
              !web_control_number(root, "altitude_accuracy_m", &altitude_accuracy)))) {
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        }
        if (altitude_value == NULL && altitude_accuracy_value != NULL) {
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        }
        params.has_location = true;
        params.latitude = latitude;
        params.longitude = longitude;
        params.accuracy_m = accuracy;
        params.altitude_valid = altitude_value != NULL;
        params.altitude_m = altitude;
        params.altitude_accuracy_m = altitude_accuracy;
        params.captured_at_ms = (uint64_t)captured_at_ms;
    }
    return workflow_service_command(command, &params);
}

static esp_err_t web_control_calibrate_heights(httpd_req_t *request)
{
    measurement_service_snapshot_t measurement;
    esp_err_t err = measurement_service_get_snapshot(&measurement);
    if (err != ESP_OK) {
        return web_control_send_result(request, "calibrate_heights", err, NULL);
    }
    if (measurement.state == MEASUREMENT_SERVICE_STARTING ||
        measurement.state == MEASUREMENT_SERVICE_RECORDING ||
        measurement.state == MEASUREMENT_SERVICE_STOPPING) {
        return web_control_send_height_calibration_result(
            request,
            0U,
            NULL,
            "recording_active");
    }

    uint8_t channel_mask = 0U;
    uint16_t generation = 0U;
    err = workflow_service_capture_height_references(&channel_mask, &generation);
    if (err == ESP_ERR_NOT_FOUND) {
        return web_control_send_height_calibration_result(request, 0U, "no_remotes", NULL);
    }
    if (err != ESP_OK) {
        return web_control_send_result(request, "calibrate_heights", err, NULL);
    }
    return web_control_send_height_calibration_result(request, channel_mask, NULL, NULL);
}


static bool web_control_scan_lease_options(
    httpd_req_t *request,
    char action[WEB_CONTROL_SCAN_ACTION_SIZE],
    char object_id[OBJECT_STORE_ID_SIZE]
)
{
    if (request == NULL || action == NULL || object_id == NULL) return false;

    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length >= WEB_CONTROL_SCAN_QUERY_SIZE) return false;

    char query[WEB_CONTROL_SCAN_QUERY_SIZE];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, "scan", action, WEB_CONTROL_SCAN_ACTION_SIZE) != ESP_OK) return false;

    if (strcmp(action, "save") != 0 && strcmp(action, "load") != 0) return false;
    if (httpd_query_key_value(query, "object_id", object_id, OBJECT_STORE_ID_SIZE) != ESP_OK) return false;
    return object_format_id_is_valid(object_id);
}


static bool web_control_scan_uses_bulk_admission(httpd_req_t *request)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length >= WEB_CONTROL_SCAN_QUERY_SIZE) return false;

    char query[WEB_CONTROL_SCAN_QUERY_SIZE];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return false;

    char kind[16];
    const esp_err_t err = httpd_query_key_value(query, "kind", kind, sizeof(kind));
    return err == ESP_ERR_NOT_FOUND ||
        (err == ESP_OK && (strcmp(kind, "scan") == 0 || strcmp(kind, "occupancy") == 0));
}


typedef struct
{
    const char *object_id;
    object_store_lease_t *lease;
    bool invoked;

} web_control_scan_lease_context_t;


static esp_err_t web_control_scan_acquire_lease(void *context)
{
    web_control_scan_lease_context_t *lease = context;
    if (lease == NULL) return ESP_ERR_INVALID_ARG;
    lease->invoked = true;
    return object_store_lease_acquire(lease->object_id, lease->lease);
}


static esp_err_t web_control_scan_try_handle(
    httpd_req_t *request,
    bool *handled
)
{
    char scan_action[WEB_CONTROL_SCAN_ACTION_SIZE] = {0};
    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    if (!web_control_scan_lease_options(request, scan_action, object_id)) {
        return web_scan_store_try_handle(request, handled);
    }

    object_store_lease_t lease = {0};
    esp_err_t err;
    if (web_control_scan_uses_bulk_admission(request)) {
        web_control_scan_lease_context_t lease_context = {
            .object_id = object_id,
            .lease = &lease,
            .invoked = false,
        };
        err = storage_bulk_admission_run_chunk(
            web_control_scan_acquire_lease,
            &lease_context
        );
        if (err == ESP_ERR_INVALID_STATE && !lease_context.invoked) {
            return web_scan_store_try_handle(request, handled);
        }
    }
    else {
        err = object_store_lease_acquire(object_id, &lease);
    }
    if (err != ESP_OK) {
        *handled = true;
        char action_name[32];
        (void)snprintf(action_name, sizeof(action_name), "scan_%s", scan_action);
        return web_control_send_result(request, action_name, err, object_id);
    }

    /* The underlying scan handler re-validates the object/profile after this
     * reservation, opens/closes every long-lived FILE while the lease remains
     * held, and keeps all socket waits outside storage_manager as before. */
    const esp_err_t scan_err = web_scan_store_try_handle(request, handled);
    const esp_err_t release_err = object_store_lease_release(&lease);
    if (release_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Scan object lease release failed object=%s err=%s",
            object_id,
            esp_err_to_name(release_err)
        );
        return scan_err != ESP_OK ? scan_err : release_err;
    }
    return scan_err;
}


static const char *web_control_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool web_control_number(cJSON *root, const char *key, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
    *value = item->valuedouble;
    return true;
}

static bool web_control_pod_index(double value, size_t *pod_index)
{
    if (
        pod_index == NULL ||
        !isfinite(value) ||
        value < 1.0 ||
        value > (double)OBJECT_STORE_POD_COUNT ||
        floor(value) != value
    ) {
        return false;
    }

    *pod_index = (size_t)value - 1U;
    return true;
}

static esp_err_t web_control_calculate_source(cJSON *root, const char *object_id)
{
    if (object_id == NULL) return ESP_ERR_INVALID_ARG;
    const char *session_id = web_control_string(root, "session_id");
    if (session_id == NULL) return ESP_ERR_INVALID_ARG;
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    const esp_err_t parse_err = localization_config_from_json(root, &config, &budget_ms);
    if (parse_err != ESP_OK) return parse_err;
    /* The run takes seconds and several KiB of stack: it is handed to the
     * localization_job worker, never run on this HTTPS task. The WebUI
     * polls /api/localization/runs?job=status. A second request while one
     * runs is refused (409), not queued. */
    return localization_job_submit(object_id, session_id, &config, budget_ms, NULL);
}

static esp_err_t web_control_save_calc_settings(cJSON *root, const char *object_id)
{
    if (root == NULL || object_id == NULL) return ESP_ERR_INVALID_ARG;

    const cJSON *material_item = cJSON_GetObjectItemCaseSensitive(root, "material");
    const char *material_id = "custom";
    const localization_material_preset_t *preset = NULL;
    if (material_item != NULL) {
        if (!cJSON_IsString(material_item) || material_item->valuestring == NULL ||
            strlen(material_item->valuestring) >= LOCALIZATION_MATERIAL_ID_SIZE) {
            return ESP_ERR_INVALID_ARG;
        }
        material_id = material_item->valuestring;
        if (strcmp(material_id, "custom") != 0) {
            preset = localization_material_preset_lookup(material_id);
            if (preset == NULL) return ESP_ERR_INVALID_ARG;
        }
    } else {
        const cJSON *speed = cJSON_GetObjectItemCaseSensitive(root, "propagation_speed_mps");
        const cJSON *uncertainty = cJSON_GetObjectItemCaseSensitive(
            root, "propagation_speed_uncertainty_mps");
        if (!cJSON_IsNumber(speed) || !cJSON_IsNumber(uncertainty) ||
            !isfinite(speed->valuedouble) || !isfinite(uncertainty->valuedouble)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t i = 0U; i < localization_material_preset_count(); ++i) {
            const localization_material_preset_t *candidate = localization_material_preset_at(i);
            if (speed->valuedouble == (double)candidate->propagation_speed_mps &&
                uncertainty->valuedouble == (double)candidate->propagation_speed_uncertainty_mps) {
                preset = candidate;
                material_id = candidate->id;
                break;
            }
        }
    }

    if (preset != NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "propagation_speed_mps");
        cJSON_DeleteItemFromObjectCaseSensitive(root, "propagation_speed_uncertainty_mps");
        if (cJSON_AddNumberToObject(root, "propagation_speed_mps",
                (double)preset->propagation_speed_mps) == NULL ||
            cJSON_AddNumberToObject(root, "propagation_speed_uncertainty_mps",
                (double)preset->propagation_speed_uncertainty_mps) == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        const cJSON *speed = cJSON_GetObjectItemCaseSensitive(root, "propagation_speed_mps");
        const cJSON *uncertainty = cJSON_GetObjectItemCaseSensitive(
            root, "propagation_speed_uncertainty_mps");
        if (!cJSON_IsNumber(speed) || !cJSON_IsNumber(uncertainty)) return ESP_ERR_INVALID_ARG;
    }

    if (material_item == NULL) {
        if (cJSON_AddStringToObject(root, "material", material_id) == NULL) return ESP_ERR_NO_MEM;
    }

    if (material_item != NULL) {
        char *standard_json = NULL;
        esp_err_t err = localization_config_standard_processing_to_json(&standard_json);
        if (err != ESP_OK || standard_json == NULL) return err != ESP_OK ? err : ESP_ERR_NO_MEM;
        cJSON *standard = cJSON_Parse(standard_json);
        cJSON_free(standard_json);
        if (standard == NULL) return ESP_ERR_NO_MEM;
        for (const cJSON *field = standard->child; field != NULL; field = field->next) {
            if (field->string == NULL ||
                cJSON_GetObjectItemCaseSensitive(root, field->string) != NULL) continue;
            if (cJSON_IsBool(field)) {
                if (cJSON_AddBoolToObject(root, field->string, cJSON_IsTrue(field)) == NULL) {
                    cJSON_Delete(standard);
                    return ESP_ERR_NO_MEM;
                }
            } else if (cJSON_IsNumber(field)) {
                if (cJSON_AddNumberToObject(root, field->string, field->valuedouble) == NULL) {
                    cJSON_Delete(standard);
                    return ESP_ERR_NO_MEM;
                }
            }
        }
        cJSON_Delete(standard);
    }

    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    const esp_err_t parse_err = localization_config_from_json(root, &config, &budget_ms);
    if (parse_err != ESP_OK) return parse_err;
    char *json = NULL;
    const esp_err_t serialize_err = localization_config_to_json(
        &config, material_id, budget_ms, &json);
    if (serialize_err != ESP_OK || json == NULL) {
        return serialize_err != ESP_OK ? serialize_err : ESP_ERR_NO_MEM;
    }
    const esp_err_t err = object_store_set_calc_settings_json(object_id, json);
    cJSON_free(json);
    if (err == ESP_OK) (void)workflow_service_notify_calc_settings_saved(object_id);
    return err;
}

static esp_err_t web_control_measurement_start(
    const char *object_id,
    const char **error_class_out
)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    workflow_snapshot_t workflow;
    const esp_err_t workflow_err = workflow_service_get_snapshot(&workflow);
    if (workflow_err != ESP_OK) {
        if (error_class_out != NULL) *error_class_out = "workflow_unavailable";
        return workflow_err;
    }
    if (workflow.step != WORKFLOW_STEP_RECORD_READY && workflow.step != WORKFLOW_STEP_RECORDING) {
        if (error_class_out != NULL) *error_class_out = "workflow_step_not_allowed";
        return ESP_ERR_INVALID_STATE;
    }

    /* Preserve synchronous source-error feedback; the shared worker also
     * starts the source for TFT requests and waits for its first fresh sample. */
    const esp_err_t acquisition_err = seismic_acquisition_start();
    if (acquisition_err != ESP_OK) {
        if (error_class_out != NULL) *error_class_out = "acquisition_unavailable";
        return acquisition_err;
    }
    return measurement_service_start_object_async(object_id);
}

static esp_err_t web_control_acquisition_stop(void)
{
    measurement_service_snapshot_t measurement;
    const esp_err_t err = measurement_service_get_snapshot(&measurement);
    if (err != ESP_OK) {
        return err;
    }

    if (measurement_service_snapshot_blocks_acquisition_stop(&measurement)) {
        ESP_LOGW(
            TAG,
            "Rejecting acquisition_stop while measurement state=%d busy=%d",
            (int)measurement.state,
            measurement.busy ? 1 : 0
        );
        return ESP_ERR_INVALID_STATE;
    }

    return seismic_acquisition_set_preview(SEISMIC_PREVIEW_WEB, false);
}

static esp_err_t web_control_process(
    cJSON *root,
    const char *action,
    char object_id_out[OBJECT_STORE_ID_SIZE],
    const char **error_class_out
)
{
    object_id_out[0] = '\0';
    if (error_class_out != NULL) *error_class_out = NULL;

    if (strcmp(action, "create_object") == 0) {
        const char *name = web_control_string(root, "name");

        object_store_summary_t created;
        const esp_err_t err = object_store_create_named(name, &created);
        if (err != ESP_OK) {
            return err;
        }

        snprintf(object_id_out, OBJECT_STORE_ID_SIZE, "%s", created.id);
        return ESP_OK;
    }

    const char *object_id = web_control_string(root, "object_id");

    if (strcmp(action, "activate_object") == 0) {
        if (object_id == NULL) return ESP_ERR_INVALID_ARG;
        object_store_summary_t previous_active = {0};
        const bool had_previous_active = object_store_get_active(&previous_active) == ESP_OK;
        const esp_err_t err = object_store_set_active(object_id);
        if (err == ESP_OK) {
            if (had_previous_active && strcmp(previous_active.id, object_id) != 0) {
                /* Staging is object-scoped; clear the object being left and
                 * preserve staging for the selected object's next recording. */
                const esp_err_t staging_err = web_scan_store_clear_staging(previous_active.id);
                if (staging_err != ESP_OK) {
                    ESP_LOGW(TAG, "Could not clear staging for previous object %s: %s",
                             previous_active.id, esp_err_to_name(staging_err));
                }
            }
            snprintf(object_id_out, OBJECT_STORE_ID_SIZE, "%s", object_id);
        }
        return err;
    }
    if (strcmp(action, "delete_object") == 0) {
        if (object_id == NULL) return ESP_ERR_INVALID_ARG;
        if (seismic_recorder_is_recording_object(object_id)) return ESP_ERR_INVALID_STATE;
        return object_store_delete(object_id);
    }
    if (strcmp(action, "rename_object") == 0) {
        const char *name = web_control_string(root, "name");
        return object_id == NULL
            ? ESP_ERR_INVALID_ARG
            : object_store_rename(object_id, name);
    }
    if (strcmp(action, "set_object_cable_length") == 0) {
        double pod = 0.0;
        double length = 0.0;
        size_t pod_index = 0U;
        if (
            object_id == NULL ||
            !web_control_number(root, "pod_id", &pod) ||
            !web_control_number(root, "length_m", &length) ||
            !web_control_pod_index(pod, &pod_index)
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        return object_store_set_cable_length(object_id, pod_index, (float)length);
    }
    if (strcmp(action, "set_default_cable_length") == 0) {
        double pod = 0.0;
        double length = 0.0;
        size_t pod_index = 0U;
        if (
            !web_control_number(root, "pod_id", &pod) ||
            !web_control_number(root, "length_m", &length) ||
            !web_control_pod_index(pod, &pod_index)
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        return object_store_set_cable_default(pod_index, (float)length);
    }
    if (strcmp(action, "set_object_location") == 0) {
        double latitude = 0.0;
        double longitude = 0.0;
        double accuracy = 0.0;
        double captured = 0.0;
        uint64_t captured_at_ms = 0U;
        double altitude = 0.0;
        double altitude_accuracy = 0.0;
        if (object_id == NULL ||
            !web_control_number(root, "latitude", &latitude) ||
            !web_control_number(root, "longitude", &longitude) ||
            !web_control_number(root, "accuracy_m", &accuracy) ||
            !web_control_number(root, "captured_at_ms", &captured) ||
            !json_number_to_uint64(captured, &captured_at_ms)) {
            return ESP_ERR_INVALID_ARG;
        }

        object_store_location_t location = {0};
        location.valid = true;
        location.latitude = latitude;
        location.longitude = longitude;
        location.accuracy_m = (float)accuracy;
        location.captured_at_ms = captured_at_ms;
        snprintf(location.source, sizeof(location.source), "PHONE_GPS");

        if (web_control_number(root, "altitude_m", &altitude)) {
            location.altitude_valid = true;
            location.altitude_m = altitude;
            if (web_control_number(root, "altitude_accuracy_m", &altitude_accuracy)) {
                location.altitude_accuracy_m = (float)altitude_accuracy;
            }
        }
        return object_store_set_location(object_id, &location);
    }
    /* Keep the legacy action names as compatibility aliases, but route them
     * through the same queued measurement service as the current WebUI. */
    if (
        strcmp(action, "recording_start") == 0 ||
        strcmp(action, "measurement_start") == 0
    ) {
        return web_control_measurement_start(object_id, error_class_out);
    }
    if (
        strcmp(action, "recording_stop") == 0 ||
        strcmp(action, "measurement_stop") == 0
    ) {
        return measurement_service_stop_async();
    }
    if (strcmp(action, "acquisition_start") == 0) {
        return seismic_acquisition_set_preview(SEISMIC_PREVIEW_WEB, true);
    }
    if (strcmp(action, "acquisition_stop") == 0) {
        return web_control_acquisition_stop();
    }
    if (strcmp(action, "calculate_source") == 0) {
        return web_control_calculate_source(root, object_id);
    }
    if (strcmp(action, "save_calc_settings") == 0) {
        return web_control_save_calc_settings(root, object_id);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t web_api_control_handler(httpd_req_t *request)
{
    /* This admission check deliberately precedes scan dispatch and body reads:
     * once the physical latch is visible, no new mutating request starts. */
    if (web_server_shutdown_requested()) {
        return web_control_send_result(
            request,
            "SHUTDOWN",
            ESP_ERR_INVALID_STATE,
            NULL
        );
    }

    /*
     * Large opaque 3D artifacts share this already-budgeted URI so the HTTPS
     * server does not spend another handler slot/RAM. Query-marked scan traffic
     * is streamed/binary; ordinary controls keep the small JSON path below.
     */
    bool scan_handled = false;
    esp_err_t err = web_control_scan_try_handle(request, &scan_handled);
    if (scan_handled || err != ESP_OK) return err;

    char *body = malloc(WEB_CONTROL_MAX_BODY);
    if (body == NULL) {
        return web_control_send_result(request, "INVALID", ESP_ERR_NO_MEM, NULL);
    }

    err = web_control_read_body(request, body, WEB_CONTROL_MAX_BODY);
    if (err != ESP_OK) {
        free(body);
        return web_control_send_result(request, "INVALID", err, NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);
    }

    const char *action = web_control_string(root, "action");
    if (action == NULL) {
        cJSON_Delete(root);
        return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);
    }

    char action_copy[WEB_CONTROL_ACTION_SIZE];
    const size_t action_length = strnlen(action, sizeof(action_copy));
    if (action_length == 0U || action_length >= sizeof(action_copy)) {
        cJSON_Delete(root);
        return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_SIZE, NULL);
    }
    memcpy(action_copy, action, action_length + 1U);

    /* Parsing is non-authoritative. Re-check immediately before service/storage
     * dispatch so a request that entered before the latch cannot start a new
     * mutation after the shutdown decision became visible. */
    if (web_server_shutdown_requested()) {
        cJSON_Delete(root);
        return web_control_send_result(
            request,
            action_copy,
            ESP_ERR_INVALID_STATE,
            NULL
        );
    }

    if (strcmp(action_copy, "workflow_command") == 0) {
        bool internal_command = false;
        const workflow_command_result_t result =
            web_control_workflow_command(root, &internal_command);
        cJSON_Delete(root);
        if (internal_command) return web_control_send_internal_workflow_command(request);
        return web_control_send_workflow_result(request, result);
    }

    if (strcmp(action_copy, "calibrate_heights") == 0) {
        cJSON_Delete(root);
        return web_control_calibrate_heights(request);
    }

    if (strcmp(action_copy, "scan_remote_sensors") == 0) {
        double channel = -1.0;
        uint32_t request_id = 0U;
        if (!web_control_number(root, "channel", &channel) || channel < 0.0 ||
            channel >= (double)NRF_TOPOLOGY_REMOTE_COUNT || floor(channel) != channel) {
            cJSON_Delete(root);
            return web_control_send_result(request, action_copy, ESP_ERR_INVALID_ARG, NULL);
        }
        err = nrf_topology_service_request_sensor_scan((uint8_t)channel, &request_id);
        cJSON_Delete(root);
        if (err != ESP_OK) return web_control_send_result(request, action_copy, err, NULL);
        char response[96];
        (void)snprintf(response, sizeof(response),
                       "{\"ok\":true,\"action\":\"scan_remote_sensors\",\"request_id\":%lu}",
                       (unsigned long)request_id);
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
    }

    if (strcmp(action_copy, "sensor_scan_dispatch") == 0) {
        double raw_id;
        uint64_t parsed_id;
        bool pending;
        esp_err_t dispatch_result;
        if (!web_control_number(root, "request_id", &raw_id) ||
            !json_number_to_uint64(raw_id, &parsed_id) || parsed_id == 0U || parsed_id > UINT32_MAX) {
            cJSON_Delete(root);
            return web_control_send_result(request, action_copy, ESP_ERR_INVALID_ARG, NULL);
        }
        err = nrf_topology_service_get_sensor_scan_dispatch((uint32_t)parsed_id, &pending, &dispatch_result);
        cJSON_Delete(root);
        if (err != ESP_OK) return web_control_send_result(request, action_copy, err, NULL);
        char response[120];
        (void)snprintf(response, sizeof(response),
            "{\"ok\":true,\"action\":\"sensor_scan_dispatch\",\"pending\":%s,\"dispatch_error\":%ld}",
            pending ? "true" : "false", (long)dispatch_result);
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
    }

    char object_id[OBJECT_STORE_ID_SIZE];
    const char *error_class = NULL;
    const bool object_storage_mutation =
        web_control_is_object_storage_mutation(action_copy);
    const bool object_storage_allowed =
        !object_storage_mutation || web_control_object_storage_is_idle();
    if (object_storage_allowed) {
        err = web_control_process(root, action_copy, object_id, &error_class);
    }
    else {
        object_id[0] = '\0';
        err = ESP_ERR_INVALID_STATE;
    }
    cJSON_Delete(root);

    /*
     * A failed transactional create can still leave storage uncertain if its
     * best-effort rollback also fails. Refresh after every object mutation
     * attempt rather than assuming an error means no filesystem change.
     */
    if (
        object_storage_allowed &&
        web_control_requires_object_cache_refresh(action_copy)
    ) {
        web_status_json_objects_invalidate();
        const esp_err_t refresh_err = web_status_json_objects_request_refresh();
        if (refresh_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Object cache refresh request failed: %s",
                esp_err_to_name(refresh_err)
            );
        }
    }
    else if (
        strcmp(action_copy, "activate_object") == 0 &&
        err == ESP_OK
    ) {
        web_status_json_objects_set_active(object_id);
    }

    ESP_LOGI(TAG, "%s -> %s", action_copy, esp_err_to_name(err));
    return web_control_send_result_with_error_class(
        request,
        error_class != NULL ? "measurement_start" : action_copy,
        err,
        object_id[0] == '\0' ? NULL : object_id,
        error_class
    );
}

esp_err_t web_api_control_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;
    const httpd_uri_t uri = {
        .uri = "/api/control",
        .method = HTTP_POST,
        .handler = web_api_control_handler,
        .user_ctx = NULL,
    };
    return web_server_register_uri_handler(server, &uri);
}
