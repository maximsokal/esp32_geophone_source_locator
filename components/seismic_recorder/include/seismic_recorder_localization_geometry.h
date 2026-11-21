#pragma once

#include "esp_err.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "seismic_recorder_geometry_policy.h"

/*
 * REMOTE_ONLY localization geometry. The WebUI registers the AprilTags after the recording and uploads
 * registration-v1.json into the session directory; in a REMOTE_ONLY session that file may carry an additional block
 *   "geophone_positions": { "source": "APRILTAG_CENTER_APPROX", "frame": "project_z_up_mm",
 *                           "positions": [ {"node_id":1,"tag_id":1,"x_mm":..,"y_mm":..,"z_mm":..}, ... ] }
 * next to the top-level "geometry_computed_at_ms" the registration is already bound to. The positions are the centres of
 * the geophones' tags in the project frame (Z up), offset 0.
 */

#define SEISMIC_RECORDER_REGISTRATION_FILE "registration-v1.json"
#define SEISMIC_RECORDER_REGISTRATION_MAX_BYTES 16384U

/*
 * Parse the registered positions out of a registration-v1.json text. ESP_ERR_NOT_FOUND: no geophone_positions block (an
 * ordinary registration); ESP_ERR_INVALID_RESPONSE: a block that breaks the contract (unknown source or frame, node outside
 * 1..4 or repeated, tag other than the node's own, coordinate not a whole number of millimetres within +-1000 m, no
 * geometry_computed_at_ms to bind to). Nothing is partially accepted.
 */
esp_err_t seismic_recorder_registered_positions_parse(const char *json, seismic_recorder_registered_positions_t *out);

/* seismic_recorder_read_session_registration_scan_id() is declared in seismic_recorder.h, next to the tag layout reader. */
