#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The tag-derived sensor layout (tag-positions-v1.json), computed by the WebUI on the phone and stored in the session directory by the
 * scan store (kind=tags). The ESP32 only stores, parses and uses it: it is an ALTERNATIVE to the Channel Sounding / barometer layout and never
 * replaces the stored one. Positions are final: integer millimetres in the MAIN-origin sensor frame (X/Y horizontal, Z up, the scan's
 * azimuth and handedness); `main_scan_mm` is MAIN's position in the scan's project frame, so scan = layout + main_scan_mm.
 * Indexed by node id (0 = MAIN, k = G<k>); the tag of node k is tag k.
 */
#define SEISMIC_RECORDER_TAG_POSITIONS_FILE "tag-positions-v1.json"
#define SEISMIC_RECORDER_TAG_POSITIONS_MAX_BYTES 4096U
#define SEISMIC_RECORDER_TAG_LAYOUT_NODE_COUNT 5U
#define SEISMIC_RECORDER_TAG_LAYOUT_SCAN_ID_MAX 48U
/* A localization needs at least three participating remotes (LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D); a tag layout is used only if it has them. */
#define SEISMIC_RECORDER_TAG_LAYOUT_MIN_REMOTES 3U

typedef struct
{
    bool valid;
    uint8_t node_id;
    uint8_t tag_id;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    /* 1-sigma 3D position uncertainty relative to MAIN, millimetres (0 for MAIN). */
    int32_t uncertainty_mm;
    /* Confirmed from several viewpoints; false = a preliminary position. */
    bool confirmed;
    uint32_t observations;
    uint32_t views;
} seismic_recorder_tag_node_t;

typedef struct
{
    bool available;
    /* The scan these tags were seen in (scan-crc32:...), empty when the phone did not know it yet. */
    char scan_frame_id[SEISMIC_RECORDER_TAG_LAYOUT_SCAN_ID_MAX];
    uint64_t created_at_ms;
    int32_t main_scan_mm[3];
    int32_t weakest_main_triangle_height_mm;
    size_t node_count;
    seismic_recorder_tag_node_t nodes[SEISMIC_RECORDER_TAG_LAYOUT_NODE_COUNT];
} seismic_recorder_tag_layout_t;

/*
 * Parse tag-positions-v1.json. Strict, nothing is partially accepted: ESP_ERR_INVALID_RESPONSE for any unknown field, wrong schema/kind/frame,
 * non-integer or out-of-range value, node outside 0..4 or repeated, a tag other than the node's own, fewer than 3 or more than 5 nodes, MAIN missing or
 * not the origin with zero uncertainty, or two nodes closer than 10 mm. `out` is zeroed (available = false) unless the file is valid.
 * Self-contained (cJSON only), so the scan store can check an upload with exactly the rules the localization applies when it reads the file.
 */
esp_err_t seismic_recorder_tag_layout_parse(const char *json, seismic_recorder_tag_layout_t *out);
