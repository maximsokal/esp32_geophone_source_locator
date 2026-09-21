#include "app_pod_status_local_position_policy.h"

app_pod_status_local_position_reading_t app_pod_status_bridge_compute_local_position_reading(
    const object_store_pod_geometry_t *pod)
{
    const app_pod_status_local_position_reading_t invalid = {0.0f, 0.0f, false};

    if (pod == NULL || !pod->computed) {
        return invalid;
    }

    const app_pod_status_local_position_reading_t reading = {
        (float)pod->x_mm / 1000.0f,
        (float)pod->y_mm / 1000.0f,
        true,
    };
    return reading;
}
