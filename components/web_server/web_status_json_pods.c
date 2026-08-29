#include "web_server_internal.h"


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

    cJSON_AddBoolToObject(
        object,
        "valid",
        barometer->valid
    );

    cJSON_AddNumberToObject(
        object,
        "pressure_pa",
        barometer->pressure_pa
    );

    cJSON_AddNumberToObject(
        object,
        "relative_altitude_cm",
        barometer->relative_altitude_cm
    );

    cJSON_AddNumberToObject(
        object,
        "reference_pressure_pa",
        barometer->reference_pressure_pa
    );

    return true;
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

    cJSON_AddBoolToObject(
        object,
        "valid",
        imu->valid
    );

    cJSON_AddNumberToObject(
        object,
        "roll_deg",
        imu->roll_deg
    );

    cJSON_AddNumberToObject(
        object,
        "pitch_deg",
        imu->pitch_deg
    );

    cJSON_AddNumberToObject(
        object,
        "yaw_relative_deg",
        imu->yaw_relative_deg
    );

    return true;
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

    cJSON_AddBoolToObject(
        object,
        "valid",
        geophone->valid
    );

    cJSON_AddNumberToObject(
        object,
        "instant_uv",
        geophone->instant_uv
    );

    cJSON_AddNumberToObject(
        object,
        "rms_uv",
        geophone->rms_uv
    );

    cJSON_AddNumberToObject(
        object,
        "peak_uv",
        geophone->peak_uv
    );

    cJSON_AddNumberToObject(
        object,
        "noise_rms_uv",
        geophone->noise_rms_uv
    );

    cJSON_AddNumberToObject(
        object,
        "snr_db",
        geophone->snr_db
    );

    return true;
}


bool web_status_json_add_pods(
    cJSON *root,
    const device_status_snapshot_t *status
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

        cJSON_AddItemToArray(
            pods,
            pod_object
        );

        cJSON_AddNumberToObject(
            pod_object,
            "id",
            i + 1
        );

        cJSON_AddStringToObject(
            pod_object,
            "state",
            device_status_pod_name(
                pod->state
            )
        );

        if (
            !web_status_json_add_barometer(
                pod_object,
                &pod->barometer
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
    }

    return true;
}
