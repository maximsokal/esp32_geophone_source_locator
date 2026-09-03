#include "web_server_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "object_store.h"
#include "seismic_acquisition.h"
#include "esp_log.h"

#define WEB_CONTROL_MAX_BODY 1024

static const char *TAG = "WEB_API_CONTROL";

static esp_err_t web_control_read_body(
    httpd_req_t *request,
    char *buffer,
    size_t buffer_size
)
{
    if (request->content_len <= 0 || request->content_len >= (int)buffer_size) {
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

static esp_err_t web_control_process(
    cJSON *root,
    const char *action,
    char object_id_out[OBJECT_STORE_ID_SIZE]
)
{
    object_id_out[0] = '\0';

    if (strcmp(action, "create_object") == 0) {
        const char *name = web_control_string(root, "name");
        if (name != NULL && (name[0] == '\0' || strlen(name) >= OBJECT_STORE_NAME_SIZE)) {
            return ESP_ERR_INVALID_SIZE;
        }

        object_store_summary_t created;
        esp_err_t err = object_store_create(&created);
        if (err == ESP_OK) {
            snprintf(object_id_out, OBJECT_STORE_ID_SIZE, "%s", created.id);
            if (name != NULL) err = object_store_rename(created.id, name);
        }
        return err;
    }

    const char *object_id = web_control_string(root, "object_id");

    if (strcmp(action, "activate_object") == 0) {
        return object_id == NULL ? ESP_ERR_INVALID_ARG : object_store_set_active(object_id);
    }
    if (strcmp(action, "delete_object") == 0) {
        return object_id == NULL ? ESP_ERR_INVALID_ARG : object_store_delete(object_id);
    }
    if (strcmp(action, "rename_object") == 0) {
        const char *name = web_control_string(root, "name");
        return object_id == NULL || name == NULL
            ? ESP_ERR_INVALID_ARG
            : object_store_rename(object_id, name);
    }
    if (strcmp(action, "set_object_cable_length") == 0) {
        double pod = 0.0;
        double length = 0.0;
        if (object_id == NULL ||
            !web_control_number(root, "pod_id", &pod) ||
            !web_control_number(root, "length_m", &length) ||
            pod < 1.0 || pod > OBJECT_STORE_POD_COUNT) {
            return ESP_ERR_INVALID_ARG;
        }
        return object_store_set_cable_length(object_id, (size_t)pod - 1U, (float)length);
    }
    if (strcmp(action, "set_default_cable_length") == 0) {
        double pod = 0.0;
        double length = 0.0;
        if (!web_control_number(root, "pod_id", &pod) ||
            !web_control_number(root, "length_m", &length) ||
            pod < 1.0 || pod > OBJECT_STORE_POD_COUNT) {
            return ESP_ERR_INVALID_ARG;
        }
        return object_store_set_cable_default((size_t)pod - 1U, (float)length);
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
    if (strcmp(action, "acquisition_start") == 0) {
        return seismic_acquisition_start();
    }
    if (strcmp(action, "acquisition_stop") == 0) {
        seismic_acquisition_stop();
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t web_api_control_handler(httpd_req_t *request)
{
    char body[WEB_CONTROL_MAX_BODY];
    esp_err_t err = web_control_read_body(request, body, sizeof(body));
    if (err != ESP_OK) return web_control_send_result(request, "INVALID", err, NULL);

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);

    const char *action = web_control_string(root, "action");
    if (action == NULL) {
        cJSON_Delete(root);
        return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);
    }

    char action_copy[48];
    snprintf(action_copy, sizeof(action_copy), "%s", action);
    char object_id[OBJECT_STORE_ID_SIZE];
    err = web_control_process(root, action_copy, object_id);
    cJSON_Delete(root);

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
    return httpd_register_uri_handler(server, &uri);
}
