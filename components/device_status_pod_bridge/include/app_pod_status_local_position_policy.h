#pragma once

#include <stdbool.h>

#include "object_store.h"

typedef struct {
    float local_x_m;
    float local_y_m;
    bool valid;
} app_pod_status_local_position_reading_t;

/*
 * Pure mapping from one object_store per-pod geometry entry (MAX-118's
 * Channel-Sounding-derived relative X/Y, in millimeters) to what the pod
 * status bridge publishes through device_status_set_pod_local_position().
 * Valid only when that pod's geometry was actually solved (computed ==
 * true); a NULL pod (no active object, or its profile fetch failed this
 * cycle) or an unsolved pod both return a zeroed, invalid reading rather
 * than a stale or fabricated position.
 */
app_pod_status_local_position_reading_t app_pod_status_bridge_compute_local_position_reading(
    const object_store_pod_geometry_t *pod);
