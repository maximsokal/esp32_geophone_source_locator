#include "web_server_internal.h"

#include "esp_timer.h"
#include "nrf_topology_service.h"


static bool web_status_json_add_capabilities(
    cJSON *pod_object,
    const device_pod_capabilities_t *capabilities
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "capabilities");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "barometer", capabilities->barometer) != NULL &&
        cJSON_AddBoolToObject(object, "environment", capabilities->environment) != NULL &&
        cJSON_AddBoolToObject(object, "imu", capabilities->imu) != NULL &&
        cJSON_AddBoolToObject(object, "geophone", capabilities->geophone) != NULL &&
        cJSON_AddBoolToObject(object, "local_position", capabilities->local_position) != NULL &&
        cJSON_AddBoolToObject(object, "global_position", capabilities->global_position) != NULL;
}


static bool web_status_json_add_barometer(
    cJSON *pod_object,
    const device_barometer_status_t *barometer
)
{
    cJSON *object =
        cJSON_AddObjectToObject(
            pod_object,
            "barometer"
        );

    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", barometer->valid) != NULL &&
        cJSON_AddNumberToObject(object, "pressure_pa", barometer->pressure_pa) != NULL &&
        cJSON_AddNumberToObject(object, "relative_altitude_cm", barometer->relative_altitude_cm) != NULL &&
        cJSON_AddNumberToObject(object, "reference_pressure_pa", barometer->reference_pressure_pa) != NULL;
}


static const char *web_status_json_height_reference_state_name(
    device_height_reference_state_t state)
{
    switch (state) {
        case DEVICE_HEIGHT_REFERENCE_VALID: return "valid";
        case DEVICE_HEIGHT_REFERENCE_REJECTED_MOVING: return "rejected_moving";
        case DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID: return "sensor_invalid";
        case DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED:
        default: return "not_established";
    }
}


static bool web_status_json_add_height_reference(
    cJSON *pod_object,
    const device_height_reference_status_t *height_reference)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "height_reference");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddStringToObject(
            object,
            "state",
            web_status_json_height_reference_state_name(height_reference->state)) != NULL &&
        cJSON_AddNumberToObject(object, "generation", height_reference->generation) != NULL &&
        cJSON_AddNumberToObject(object, "relative_z_mm", height_reference->relative_z_mm) != NULL;
}


static bool web_status_json_add_environment(
    cJSON *pod_object,
    const device_pod_environment_status_t *environment
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "environment");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", environment->valid) != NULL &&
        cJSON_AddBoolToObject(object, "temperature_valid", environment->temperature_valid) != NULL &&
        cJSON_AddNumberToObject(object, "temperature_c", environment->temperature_c) != NULL &&
        cJSON_AddBoolToObject(object, "humidity_valid", environment->humidity_valid) != NULL &&
        cJSON_AddNumberToObject(object, "humidity_percent", environment->humidity_percent) != NULL;
}


static bool web_status_json_add_imu(
    cJSON *pod_object,
    const device_imu_status_t *imu
)
{
    cJSON *object =
        cJSON_AddObjectToObject(
            pod_object,
            "imu"
        );

    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", imu->valid) != NULL &&
        cJSON_AddNumberToObject(object, "roll_deg", imu->roll_deg) != NULL &&
        cJSON_AddNumberToObject(object, "pitch_deg", imu->pitch_deg) != NULL &&
        cJSON_AddNumberToObject(object, "yaw_relative_deg", imu->yaw_relative_deg) != NULL;
}


static bool web_status_json_add_geophone(
    cJSON *pod_object,
    const device_geophone_status_t *geophone
)
{
    cJSON *object =
        cJSON_AddObjectToObject(
            pod_object,
            "geophone"
        );

    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", geophone->valid) != NULL &&
        cJSON_AddNumberToObject(object, "instant_uv", geophone->instant_uv) != NULL &&
        cJSON_AddNumberToObject(object, "rms_uv", geophone->rms_uv) != NULL &&
        cJSON_AddNumberToObject(object, "peak_uv", geophone->peak_uv) != NULL &&
        cJSON_AddNumberToObject(object, "noise_rms_uv", geophone->noise_rms_uv) != NULL &&
        cJSON_AddNumberToObject(object, "snr_db", geophone->snr_db) != NULL;
}


static bool web_status_json_add_position(
    cJSON *pod_object,
    const device_pod_position_status_t *position
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "position");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "local_valid", position->local_valid) != NULL &&
        cJSON_AddNumberToObject(object, "local_x_m", position->local_x_m) != NULL &&
        cJSON_AddNumberToObject(object, "local_y_m", position->local_y_m) != NULL &&
        cJSON_AddBoolToObject(object, "global_valid", position->global_valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", position->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", position->longitude) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", position->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", position->altitude_m) != NULL;
}


bool web_status_json_add_pods(
    cJSON *root,
    const device_status_snapshot_t *status,
    bool recording
)
{
    cJSON *pods =
        cJSON_AddArrayToObject(
            root,
            "pods"
        );

    if (pods == NULL) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const uint64_t now_ms = now_us >= 0 ? (uint64_t)now_us / 1000U : 0U;

    for (
        size_t i = 0;
        i < DEVICE_STATUS_POD_COUNT;
        ++i
    ) {
        const device_pod_status_t *pod =
            &status->pods[i];

        cJSON *pod_object =
            cJSON_CreateObject();

        if (pod_object == NULL) {
            return false;
        }

        if (!cJSON_AddItemToArray(pods, pod_object)) {
            cJSON_Delete(pod_object);
            return false;
        }

        if (
            cJSON_AddNumberToObject(pod_object, "id", i + 1) == NULL ||
            cJSON_AddStringToObject(
                pod_object,
                "state",
                device_status_pod_name(pod->state)
            ) == NULL
        ) {
            return false;
        }

        nrf_node_link_status_input_t link_input = {0};
        const esp_err_t link_snapshot_err =
            nrf_topology_service_get_node_status_snapshot(
                (uint8_t)(i + 1U),
                &link_input);
        const nrf_node_link_status_t link_status = link_snapshot_err == ESP_OK
            ? nrf_node_link_status_evaluate(&link_input, now_ms, recording)
            : NRF_NODE_LINK_STATUS_NOT_CONNECTED;
        if (cJSON_AddStringToObject(
                pod_object,
                "node_link_status",
                nrf_node_link_status_name(link_status)) == NULL) {
            return false;
        }

        if (
            !web_status_json_add_capabilities(
                pod_object,
                &pod->capabilities
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_barometer(
                pod_object,
                &pod->barometer
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_height_reference(
                pod_object,
                &pod->height_reference
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_environment(
                pod_object,
                &pod->environment
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_imu(
                pod_object,
                &pod->imu
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_geophone(
                pod_object,
                &pod->geophone
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_position(
                pod_object,
                &pod->position
            )
        ) {
            return false;
        }
    }

    return true;
}
