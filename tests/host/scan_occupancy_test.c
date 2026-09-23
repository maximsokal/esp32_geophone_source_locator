#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_occupancy.h"

/*
 * MAX-162: parses the golden vector the WebUI encoder produces
 * (webui/tests/xr/occupancySolverArtifact.test.ts writes/verifies
 * tests/vectors/scan_occupancy_v1.bin), so both sides agree on the format.
 *
 * Golden content: voxel 0.18 m; FREE (0,0,0) and (1,-1,2); OCCUPIED
 * (0,1,-1); envelope [-1,-1,-0.5]..[1,1,0.5]; 2 m below surface;
 * registration: translation +0.5 m in x, geometry version 123456.
 */

static uint8_t *read_vector(size_t *length)
{
    static const char *const candidates[] = {
        "tests/vectors/scan_occupancy_v1.bin",
        "../../../tests/vectors/scan_occupancy_v1.bin",
    };
    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        FILE *file = fopen(candidates[i], "rb");
        if (file == NULL) {
            continue;
        }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        uint8_t *data = malloc((size_t)size);
        assert(data != NULL);
        assert(fread(data, 1U, (size_t)size, file) == (size_t)size);
        fclose(file);
        *length = (size_t)size;
        return data;
    }
    assert(!"golden vector tests/vectors/scan_occupancy_v1.bin not found");
    return NULL;
}


static void test_golden_vector_parses_and_classifies(void)
{
    size_t length = 0U;
    uint8_t *data = read_vector(&length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);

    assert(strcmp(occupancy.frame_id, "scan-crc32:deadbeef") == 0);
    assert(occupancy.voxel_size_m == 0.18);
    assert(occupancy.below_surface_m == 2.0);
    assert(occupancy.free_count == 2U && occupancy.occupied_count == 1U);
    assert(occupancy.has_envelope);
    assert(occupancy.registration.present);
    assert(occupancy.registration.geometry_computed_at_ms == 123456U);

    const double free_point[3] = { 0.10, 0.05, 0.02 };        /* cell (0,0,0) */
    const double free_point2[3] = { 0.20, -0.10, 0.40 };      /* cell (1,-1,2) -- outside envelope z but FREE wins */
    const double occupied_point[3] = { 0.05, 0.30, -0.10 };   /* cell (0,1,-1) */
    const double unknown_inside[3] = { -0.50, 0.50, 0.00 };
    const double unknown_below[3] = { 0.00, 0.00, -1.80 };    /* within 2 m under the envelope */
    const double outside_below[3] = { 0.00, 0.00, -3.00 };
    const double outside_side[3] = { 5.00, 0.00, 0.00 };
    assert(scan_occupancy_classify(&occupancy, free_point) == SCAN_OCCUPANCY_FREE);
    assert(scan_occupancy_classify(&occupancy, free_point2) == SCAN_OCCUPANCY_FREE);
    assert(scan_occupancy_classify(&occupancy, occupied_point) == SCAN_OCCUPANCY_OCCUPIED);
    assert(scan_occupancy_classify(&occupancy, unknown_inside) == SCAN_OCCUPANCY_UNKNOWN);
    assert(scan_occupancy_classify(&occupancy, unknown_below) == SCAN_OCCUPANCY_UNKNOWN);
    assert(scan_occupancy_classify(&occupancy, outside_below) == SCAN_OCCUPANCY_OUTSIDE);
    assert(scan_occupancy_classify(&occupancy, outside_side) == SCAN_OCCUPANCY_OUTSIDE);

    /* Registration binds to the exact geometry version; a stale one fails closed. */
    assert(scan_occupancy_registration_usable(&occupancy, 123456U));
    assert(!scan_occupancy_registration_usable(&occupancy, 123457U));
    assert(!scan_occupancy_registration_usable(&occupancy, 0U));

    const double sensor[3] = { 1.0, 2.0, 3.0 };
    double scan[3];
    scan_occupancy_sensor_to_scan(&occupancy, sensor, scan);
    assert(scan[0] == 1.5 && scan[1] == 2.0 && scan[2] == 3.0);

    scan_occupancy_free(&occupancy);
    free(data);
}


static void test_corruption_and_bad_registration_fail_closed(void)
{
    size_t length = 0U;
    uint8_t *data = read_vector(&length);
    scan_occupancy_t occupancy;

    data[length - 1U] ^= 0xFFU;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_ERR_INVALID_CRC);
    data[length - 1U] ^= 0xFFU;

    data[0] = 'X';
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_ERR_INVALID_VERSION);
    data[0] = 'G';

    assert(scan_occupancy_parse(data, length - 1U, &occupancy) == ESP_ERR_INVALID_SIZE);

    /* A non-rigid (scaled) transform is never usable. */
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    occupancy.registration.sensor_to_scan[0] = 2.0;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    /* A mirror (det = -1) is allowed: CS layouts may be reflected. */
    occupancy.registration.sensor_to_scan[0] = -1.0;
    assert(scan_occupancy_registration_usable(&occupancy, 123456U));
    /* No registration at all: fail closed. */
    occupancy.registration.present = false;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    scan_occupancy_free(&occupancy);
    free(data);
}


int main(void)
{
    test_golden_vector_parses_and_classifies();
    test_corruption_and_bad_registration_fail_closed();
    return 0;
}
