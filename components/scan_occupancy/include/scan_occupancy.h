#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * MAX-162: ESP32-side reader for the compact FREE/OCCUPIED occupancy
 * artifact the WebUI derives from a saved scan (layout documented in
 * webui/src/xr/occupancySolverArtifact.ts and the legacy/project-frame golden
 * vectors tests/vectors/scan_occupancy_v1.bin and scan_occupancy_v2.bin).
 * Pure: no file I/O, no RTOS.
 *
 * Coordinates are the scan's project frame (meters, Z up). A voxel index is
 * floor(coordinate / voxel_size_m). UNKNOWN is never stored: a cell inside
 * the candidate volume (envelope extended below_surface_m downward) that is
 * neither FREE nor OCCUPIED is UNKNOWN.
 */

#define SCAN_OCCUPANCY_SCHEMA 2U
#define SCAN_OCCUPANCY_LEGACY_SCHEMA 1U
#define SCAN_OCCUPANCY_MAX_BYTES (1024U * 1024U)
#define SCAN_OCCUPANCY_FRAME_ID_SIZE 64U
#define SCAN_OCCUPANCY_SOURCE_SIZE 32U

typedef enum
{
    SCAN_OCCUPANCY_UNKNOWN = 0,
    SCAN_OCCUPANCY_FREE,
    SCAN_OCCUPANCY_OCCUPIED,
    /* Outside the scanned candidate volume: nothing was measured there and
     * it is not part of the scanned object. */
    SCAN_OCCUPANCY_OUTSIDE,

} scan_occupancy_class_t;

typedef struct
{
    bool present;
    char source[SCAN_OCCUPANCY_SOURCE_SIZE];
    uint64_t geometry_computed_at_ms;
    /* Schema 2: row-major sensor Z-up -> project Z-up transform. A schema 1
     * registration is parsed for compatibility but is always marked absent. */
    double sensor_to_scan[16];
    double residual_m;
    /* Schema 2 quality bound: finite and > 0; residual_m must not exceed it. */
    double threshold_m;

} scan_occupancy_registration_t;

typedef struct
{
    char frame_id[SCAN_OCCUPANCY_FRAME_ID_SIZE];
    double voxel_size_m;
    double below_surface_m;
    bool has_envelope;
    /* SHA-256 of the scan this occupancy was built for (issue #34); absent in older artifacts (then the CRC32 frame_id is the only binding). */
    bool has_scan_sha256;
    unsigned char scan_sha256[32];
    double envelope_min[3];
    double envelope_max[3];
    bool saturated;
    uint64_t created_at_ms;
    scan_occupancy_registration_t registration;

    /* Sorted packed voxel keys (heap-owned; release with scan_occupancy_free). */
    int64_t *free_keys;
    size_t free_count;
    int64_t *occupied_keys;
    size_t occupied_count;

} scan_occupancy_t;


/*
 * Parses and validates (magic, length table, CRC32, schema, counts) an
 * artifact. On success `out` owns heap memory; on failure `out` holds
 * nothing to free. ESP_ERR_INVALID_CRC for a checksum mismatch,
 * ESP_ERR_INVALID_VERSION for a foreign magic/schema, ESP_ERR_INVALID_SIZE
 * for an inconsistent length table, ESP_ERR_INVALID_ARG otherwise.
 */
esp_err_t scan_occupancy_parse(
    const uint8_t *data,
    size_t length,
    scan_occupancy_t *out
);

void scan_occupancy_free(scan_occupancy_t *occupancy);

/* Classifies a point given in the project Z-up scan frame. */
scan_occupancy_class_t scan_occupancy_classify(
    const scan_occupancy_t *occupancy,
    const double point_scan[3]
);

/*
 * The occupancy constraint may be applied to a localization run only when
 * the artifact carries a registration made against exactly this session's
 * frozen geometry version, whose residual is within its positive finite
 * threshold, and whose transform is a finite proper rigid motion. The
 * rotation block must be orthonormal and have determinant +1 within 1e-3;
 * reflections fail closed.
 */
bool scan_occupancy_registration_usable(
    const scan_occupancy_t *occupancy,
    uint64_t geometry_computed_at_ms
);

/* Returns NULL when usable, otherwise a stable invalid-registration reason. */
const char *scan_occupancy_registration_reason(
    const scan_occupancy_t *occupancy,
    uint64_t geometry_computed_at_ms
);

/* Applies a usable schema 2 transform: sensor Z-up -> project Z-up. */
void scan_occupancy_sensor_to_scan(
    const scan_occupancy_t *occupancy,
    const double point_sensor[3],
    double point_scan[3]
);
