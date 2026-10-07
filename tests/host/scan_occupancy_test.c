#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_occupancy.h"

/*
 * MAX-162: parses the golden vector the WebUI encoder produces
 * (webui/tests/xr/occupancySolverArtifact.test.ts writes/verifies the v2
 * vector), so both sides agree on the format. The v1 file remains a legacy
 * reader fixture and its registration must be ignored.
 *
 * The legacy v1 fixture has a small FREE/OCCUPIED grid. The project-frame
 * v2 fixture uses a fitted 90-degree yaw and local-floor translation (1,2,3),
 * which maps sensor origin to project [1,-3,2].
 */

static uint8_t *read_vector(bool project_v2, size_t *length)
{
    static const char *const v1_candidates[] = {
        "tests/vectors/scan_occupancy_v1.bin",
        "../../../tests/vectors/scan_occupancy_v1.bin",
    };
    static const char *const v2_candidates[] = {
        "tests/vectors/scan_occupancy_v2.bin",
        "../../../tests/vectors/scan_occupancy_v2.bin",
    };
    const char *const *candidates = project_v2 ? v2_candidates : v1_candidates;
    const size_t candidate_count = project_v2
        ? sizeof(v2_candidates) / sizeof(v2_candidates[0])
        : sizeof(v1_candidates) / sizeof(v1_candidates[0]);
    for (size_t i = 0U; i < candidate_count; ++i) {
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
    assert(!"occupancy golden vector not found");
    return NULL;
}


static void test_golden_vector_parses_and_classifies(void)
{
    size_t length = 0U;
    uint8_t *data = read_vector(false, &length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);

    assert(strcmp(occupancy.frame_id, "scan-crc32:deadbeef") == 0);
    assert(occupancy.voxel_size_m == 0.18);
    assert(occupancy.below_surface_m == 2.0);
    assert(occupancy.free_count == 2U && occupancy.occupied_count == 1U);
    assert(occupancy.has_envelope);
    assert(!occupancy.registration.present);
    assert(occupancy.registration.geometry_computed_at_ms == 0U);
    assert(occupancy.registration.threshold_m == 0.0);

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

    /* Legacy schema 1 keeps its voxels but never applies its ambiguous frame. */
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    assert(!scan_occupancy_registration_usable(&occupancy, 123457U));
    assert(!scan_occupancy_registration_usable(&occupancy, 0U));

    scan_occupancy_free(&occupancy);
    free(data);
}


static void test_project_frame_v2_registration_and_candidate_classification(void)
{
    size_t length = 0U;
    uint8_t *data = read_vector(true, &length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    assert(occupancy.registration.present);
    assert(occupancy.registration.geometry_computed_at_ms == 123456U);
    assert(occupancy.registration.threshold_m > 0.0);
    assert(scan_occupancy_registration_usable(&occupancy, 123456U));

    const double sensor_origin[3] = { 0.0, 0.0, 0.0 };
    double project[3];
    scan_occupancy_sensor_to_scan(&occupancy, sensor_origin, project);
    assert(fabs(project[0] - 1.0) < 1e-6);
    assert(fabs(project[1] + 3.0) < 1e-6);
    assert(fabs(project[2] - 2.0) < 1e-6);
    assert(scan_occupancy_classify(&occupancy, project) == SCAN_OCCUPANCY_FREE);

    /* The unconverted local-floor result occupies a different, OCCUPIED cell. */
    const double unconverted[3] = { 1.0, 2.0, 3.0 };
    assert(scan_occupancy_classify(&occupancy, unconverted) == SCAN_OCCUPANCY_OCCUPIED);
    scan_occupancy_free(&occupancy);
    free(data);
}


static uint32_t test_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void write_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
    data[offset + 2U] = (uint8_t)(value >> 16U);
    data[offset + 3U] = (uint8_t)(value >> 24U);
}

static void write_u16(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
}

static uint8_t *make_header_only_artifact(const char *header, size_t *length_out)
{
    const size_t header_length = strlen(header);
    const size_t length = 20U + header_length;
    uint8_t *data = calloc(length, sizeof(*data));
    assert(data != NULL);
    memcpy(data, "GOCCUPY1", 8U);
    write_u32(data, 8U, (uint32_t)header_length);
    write_u32(data, 12U, 0U);
    memcpy(data + 20U, header, header_length);
    write_u32(data, 16U, test_crc32(data + 20U, length - 20U));
    *length_out = length;
    return data;
}


static void test_registration_quality_requires_positive_threshold(void)
{
    scan_occupancy_t occupancy = { 0 };
    occupancy.registration.present = true;
    occupancy.registration.geometry_computed_at_ms = 123456U;
    occupancy.registration.threshold_m = 0.05;
    occupancy.registration.residual_m = 0.30;
    occupancy.registration.sensor_to_scan[0] = 1.0;
    occupancy.registration.sensor_to_scan[5] = 1.0;
    occupancy.registration.sensor_to_scan[10] = 1.0;
    occupancy.registration.sensor_to_scan[15] = 1.0;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));

    occupancy.registration.residual_m = occupancy.registration.threshold_m + 1e-12;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    occupancy.registration.residual_m = occupancy.registration.threshold_m;
    assert(scan_occupancy_registration_usable(&occupancy, 123456U));
    occupancy.registration.threshold_m = 0.0;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
}


static void test_schema2_registration_without_threshold_is_rejected(void)
{
    static const char header[] =
        "{\"schema\":2,\"frame_id\":\"scan-crc32:12345678\",\"voxel_size_m\":0.5,"
        "\"below_surface_m\":1,\"free_count\":0,\"occupied_count\":0,\"created_at_ms\":1,"
        "\"registration\":{\"source\":\"test\",\"frame\":\"project_z_up\","
        "\"geometry_computed_at_ms\":123456,\"residual_m\":0.01,"
        "\"sensor_to_scan\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}}";
    size_t length = 0U;
    uint8_t *data = make_header_only_artifact(header, &length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_ERR_INVALID_ARG);
    free(data);
}

static void test_schema1_without_registration_still_classifies(void)
{
    static const char header[] =
        "{\"schema\":1,\"frame_id\":\"legacy-empty\",\"voxel_size_m\":0.5,"
        "\"below_surface_m\":1,\"envelope\":{\"min\":[-1,-1,-1],\"max\":[1,1,1]},"
        "\"free_count\":1,\"occupied_count\":0,\"saturated\":false,"
        "\"created_at_ms\":1,\"registration\":null}";
    const size_t header_length = sizeof(header) - 1U;
    const size_t length = 20U + header_length + 6U;
    uint8_t *data = calloc(length, sizeof(*data));
    assert(data != NULL);
    memcpy(data, "GOCCUPY1", 8U);
    write_u32(data, 8U, (uint32_t)header_length);
    write_u32(data, 12U, 6U);
    memcpy(data + 20U, header, header_length);
    write_u16(data, 20U + header_length, 0U);
    write_u16(data, 20U + header_length + 2U, 0U);
    write_u16(data, 20U + header_length + 4U, 0U);
    write_u32(data, 16U, test_crc32(data + 20U, length - 20U));

    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    const double free_point[3] = { 0.1, 0.1, 0.1 };
    assert(scan_occupancy_classify(&occupancy, free_point) == SCAN_OCCUPANCY_FREE);
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    scan_occupancy_free(&occupancy);
    free(data);
}


static void test_corruption_and_bad_registration_fail_closed(void)
{
    size_t length = 0U;
    uint8_t *data = read_vector(true, &length);
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
    const double original[16] = {
        occupancy.registration.sensor_to_scan[0], occupancy.registration.sensor_to_scan[1],
        occupancy.registration.sensor_to_scan[2], occupancy.registration.sensor_to_scan[3],
        occupancy.registration.sensor_to_scan[4], occupancy.registration.sensor_to_scan[5],
        occupancy.registration.sensor_to_scan[6], occupancy.registration.sensor_to_scan[7],
        occupancy.registration.sensor_to_scan[8], occupancy.registration.sensor_to_scan[9],
        occupancy.registration.sensor_to_scan[10], occupancy.registration.sensor_to_scan[11],
        occupancy.registration.sensor_to_scan[12], occupancy.registration.sensor_to_scan[13],
        occupancy.registration.sensor_to_scan[14], occupancy.registration.sensor_to_scan[15],
    };
    occupancy.registration.sensor_to_scan[0] = 2.0;
    assert(!scan_occupancy_registration_usable(&occupancy, 123456U));
    /* A mirror (det = -1) is allowed: CS layouts may be reflected. */
    memcpy(occupancy.registration.sensor_to_scan, original, sizeof(original));
    occupancy.registration.sensor_to_scan[1] = -occupancy.registration.sensor_to_scan[1];
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
    test_project_frame_v2_registration_and_candidate_classification();
    test_schema1_without_registration_still_classifies();
    test_registration_quality_requires_positive_threshold();
    test_schema2_registration_without_threshold_is_rejected();
    test_corruption_and_bad_registration_fail_closed();
    return 0;
}
