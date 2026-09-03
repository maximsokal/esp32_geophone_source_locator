#include "web_server_internal.h"

#include <stddef.h>

#include "esp_timer.h"

static bool web_status_json_add_system(cJSON *root, const device_status_snapshot_t *status)
{
    cJSON *system = cJSON_AddObjectToObject(root, "system");
    if (system == NULL) return false;

    cJSON_AddStringToObject(system, "state", "READY");
    cJSON_AddStringToObject(system, "mode", device_status_mode_name(status->mode));
    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(system, "uptime_ms", (double)uptime_ms);
    return true;
}

static bool web_status_json_add_power(cJSON *root, const device_power_status_t *power)
{
    cJSON *object = cJSON_AddObjectToObject(root, "power");
    if (object == NULL) return false;

    cJSON_AddBoolToObject(object, "valid", power->valid);
    cJSON_AddNumberToObject(object, "voltage_v", power->voltage_v);
    cJSON_AddNumberToObject(object, "current_a", power->current_a);
    cJSON_AddNumberToObject(object, "power_w", power->power_w);

    cJSON *channels = cJSON_AddArrayToObject(object, "channels");
    if (channels == NULL) return false;
    for (size_t i = 0; i < DEVICE_STATUS_POWER_CHANNEL_COUNT; ++i) {
        const device_power_channel_status_t *channel = &power->channels[i];
        cJSON *channel_object = cJSON_CreateObject();
        if (channel_object == NULL) return false;
        cJSON_AddItemToArray(channels, channel_object);
        cJSON_AddNumberToObject(channel_object, "id", i + 1);
        cJSON_AddBoolToObject(channel_object, "valid", channel->valid);
        cJSON_AddNumberToObject(channel_object, "voltage_v", channel->voltage_v);
        cJSON_AddNumberToObject(channel_object, "current_a", channel->current_a);
        cJSON_AddNumberToObject(channel_object, "power_w", channel->power_w);
    }
    return true;
}

static bool web_status_json_add_environment(cJSON *root, const device_environment_status_t *environment)
{
    cJSON *object = cJSON_AddObjectToObject(root, "environment");
    if (object == NULL) return false;
    cJSON_AddBoolToObject(object, "valid", environment->valid);
    cJSON_AddNumberToObject(object, "temperature_c", environment->temperature_c);
    cJSON_AddNumberToObject(object, "humidity_percent", environment->humidity_percent);
    cJSON_AddNumberToObject(object, "pressure_pa", environment->pressure_pa);
    return true;
}

static bool web_status_json_add_mux(cJSON *root, const device_mux_status_t *mux)
{
    cJSON *object = cJSON_AddObjectToObject(root, "mux");
    if (object == NULL) return false;
    cJSON_AddBoolToObject(object, "valid", mux->valid);
    cJSON_AddNumberToObject(object, "active_channel", mux->active_channel);
    cJSON_AddNumberToObject(object, "active_pod", mux->active_pod);
    return true;
}

static bool web_status_json_add_acquisition(cJSON *root, const device_acquisition_status_t *acquisition)
{
    cJSON *object = cJSON_AddObjectToObject(root, "acquisition");
    if (object == NULL) return false;
    cJSON_AddBoolToObject(object, "running", acquisition->running);
    cJSON_AddNumberToObject(object, "sample_rate_hz", acquisition->sample_rate_hz);
    cJSON_AddNumberToObject(object, "samples_recorded", (double)acquisition->samples_recorded);
    cJSON_AddNumberToObject(object, "recording_ms", (double)acquisition->recording_ms);
    return true;
}

esp_err_t web_status_json_build_extended(char **json_out, bool include_objects)
{
    if (json_out == NULL) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;

    device_status_snapshot_t status;
    esp_err_t err = device_status_get_snapshot(&status);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    bool ok =
        web_status_json_add_system(root, &status) &&
        web_status_json_add_power(root, &status.power) &&
        web_status_json_add_environment(root, &status.environment) &&
        web_status_json_add_mux(root, &status.mux) &&
        web_status_json_add_pods(root, &status) &&
        web_status_json_add_acquisition(root, &status.acquisition);

    if (ok && include_objects) {
        ok = web_status_json_add_objects(root);
    }

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    *json_out = json;
    return ESP_OK;
}

esp_err_t web_status_json_build(char **json_out)
{
    return web_status_json_build_extended(json_out, false);
}

void web_status_json_free(char *json)
{
    if (json != NULL) cJSON_free(json);
}
