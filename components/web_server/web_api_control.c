#include "web_server_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object_store.h"
#include "measurement_service.h"
#include "seismic_acquisition.h"
#include "esp_log.h"

#define WEB_CONTROL_MAX_BODY 1024U
#define WEB_CONTROL_ACTION_SIZE 48U

static const char *TAG = "WEB_API_CONTROL";

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
        (action != NULL && strcmp(action, "activate_object") == 0);
}


static bool web_control_object_storage_is_idle(void)
{
    measurement_service_snapshot_t measurement;
    if (measurement_service_get_snapshot(&measurement) != ESP_OK) {
        return false;
    }

    return
        !measurement.busy &&
        (measurement.state == MEASUREMENT_SERVICE_LIVE ||
         measurement.state == MEASUREMENT_SERVICE_REVIEW ||
         measurement.state == MEASUREMENT_SERVICE_ERROR);
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
        int received = httpd_req_recv(
            request,
            buffer + offset,
            (size_t)request->content_len - offset
        );
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) return ESP_FAIL;
        offset += (size_t)received;
    }

    buffer[offset] = '\0';
    return ESP_OK;
}

static esp_err_t web_control_send_result(
    httpd_req_t *request,
    const char *action,
    esp_err_t result,
    const char *object_id
)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(root, "ok", result == ESP_OK);
    cJSON_AddStringToObject(root, "action", action == NULL ? "UNKNOWN" : action);
    if (object_id != NULL) cJSON_AddStringToObject(root, "object_id", object_id);
    if (result != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(result));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE || result == ESP_ERR_NOT_SUPPORTED) {
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

    esp_err_t send_result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_result;
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

static bool web_control_name_valid(const char *name)
{
    if (name == NULL) return false;

    const size_t length = strnlen(name, OBJECT_STORE_NAME_SIZE);
    if (length == 0U || length >= OBJECT_STORE_NAME_SIZE) return false;

    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 0x20U) return false;
    }
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

static esp_err_t web_control_measurement_start(const char *object_id)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
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

    seismic_acquisition_stop();
    return ESP_OK;
}

static esp_err_t web_control_process(
    cJSON *root,
    const char *action,
    char object_id_out[OBJECT_STORE_ID_SIZE]
)
{
    object_id_out[0] = '\0';

    if (strcmp(action, "create_object") == 0) {
        const char *name = web_control_string(root, "name");
        if (name != NULL && !web_control_name_valid(name)) {
            return ESP_ERR_INVALID_SIZE;
        }

        object_store_summary_t created;
        esp_err_t err = object_store_create(&created);
        if (err != ESP_OK) {
            return err;
        }

        const size_t id_length = strnlen(created.id, sizeof(created.id));
        if (id_length >= OBJECT_STORE_ID_SIZE) {
            (void)object_store_delete(created.id);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(object_id_out, created.id, id_length + 1U);

        if (name != NULL) {
            err = object_store_rename(created.id, name);
            if (err != ESP_OK) {
                const esp_err_t rollback_err = object_store_delete(created.id);
                if (rollback_err != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "create_object rename failed (%s), rollback failed (%s)",
                        esp_err_to_name(err),
                        esp_err_to_name(rollback_err)
                    );
                }
                object_id_out[0] = '\0';
            }
        }
        return err;
    }

    const char *object_id = web_control_string(root, "object_id");

    if (strcmp(action, "activate_object") == 0) {
        if (object_id == NULL) return ESP_ERR_INVALID_ARG;
        const esp_err_t err = object_store_set_active(object_id);
        if (err == ESP_OK) {
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
        return object_id == NULL || !web_control_name_valid(name)
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
        double altitude = 0.0;
        double altitude_accuracy = 0.0;
        if (object_id == NULL ||
            !web_control_number(root, "latitude", &latitude) ||
            !web_control_number(root, "longitude", &longitude) ||
            !web_control_number(root, "accuracy_m", &accuracy) ||
            !web_control_number(root, "captured_at_ms", &captured) ||
            captured < 0.0) {
            return ESP_ERR_INVALID_ARG;
        }

        object_store_location_t location = {0};
        location.valid = true;
        location.latitude = latitude;
        location.longitude = longitude;
        location.accuracy_m = (float)accuracy;
        location.captured_at_ms = (uint64_t)captured;
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
        return web_control_measurement_start(object_id);
    }
    if (
        strcmp(action, "recording_stop") == 0 ||
        strcmp(action, "measurement_stop") == 0
    ) {
        return measurement_service_stop_async();
    }
    if (strcmp(action, "acquisition_start") == 0) {
        return seismic_acquisition_start();
    }
    if (strcmp(action, "acquisition_stop") == 0) {
        return web_control_acquisition_stop();
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t web_api_control_handler(httpd_req_t *request)
{
    /*
     * Large opaque 3D artifacts share this already-budgeted URI so the HTTPS
     * server does not spend another handler slot/RAM. Query-marked scan traffic
     * is streamed/binary; ordinary controls keep the small JSON path below.
     */
    bool scan_handled = false;
    esp_err_t err = web_scan_store_try_handle(request, &scan_handled);
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

    char object_id[OBJECT_STORE_ID_SIZE];
    const bool object_storage_mutation =
        web_control_is_object_storage_mutation(action_copy);
    const bool object_storage_allowed =
        !object_storage_mutation || web_control_object_storage_is_idle();
    if (object_storage_allowed) {
        err = web_control_process(root, action_copy, object_id);
    }
    else {
        object_id[0] = '\0';
        err = ESP_ERR_INVALID_STATE;
    }
    cJSON_Delete(root);

    /*
     * Invalidate even after a failed compound mutation: create can partially
     * mutate storage before rollback. A cold refresh is safer than serving
     * stale metadata after that path.
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
    return web_control_send_result(
        request,
        action_copy,
        err,
        object_id[0] == '\0' ? NULL : object_id
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
