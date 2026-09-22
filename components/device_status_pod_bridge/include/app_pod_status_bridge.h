#pragma once

#include "esp_err.h"


/*
 * MAX-118: surface live per-pod barometer telemetry (already flowing through
 * nrf_topology_service's sensor cache since MAX-114) into device_status, so
 * the Measurements screen and Web status API -- both already capable of
 * displaying it, see web_status_json_add_pods() -- have something to show.
 *
 * Starts a low-priority background task that periodically reads each REMOTE
 * channel's cached sensor snapshot and republishes pressure/altitude through
 * device_status_set_pod_barometer(). Read-only with respect to
 * nrf_topology_service: never requests a refresh, never blocks acquisition.
 *
 * IMU (roll/pitch/yaw) is intentionally NOT populated here: the actual
 * orientation estimator produces a single combined
 * tilt-from-vertical metric (orientation_tilt_sin2_ppm), not decomposed
 * roll/pitch, and does not measure yaw/heading at all (Channel Sounding
 * ranging has no angle-of-arrival information -- see MAX-118's own
 * documented limitation). Populating device_status_set_pod_imu()'s
 * roll_deg/pitch_deg/yaw_relative_deg fields would mean fabricating numbers
 * the hardware does not actually measure; that needs a product decision
 * (e.g. redefine what those fields mean for this sensor set, or add a
 * tilt-only pod status field instead), not a guess made here. Likewise
 * local_position (X/Y) is read from the active object's latest frozen geometry
 * and republished every bridge cycle; it remains invalid until that geometry
 * has been computed and an active profile can be read.
 */
esp_err_t app_pod_status_bridge_start(void);

void app_pod_status_bridge_stop(void);
