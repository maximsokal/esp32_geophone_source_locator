#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_calculate_policy.h"
#include "scan_occupancy.h"

static uint32_t test_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void write_u16(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
    data[offset + 2U] = (uint8_t)(value >> 16U);
    data[offset + 3U] = (uint8_t)(value >> 24U);
}

static uint8_t *make_saturated_artifact(size_t *length_out)
{
    static const char header[] =
        "{\"schema\":2,\"frame_id\":\"saturated-test\",\"voxel_size_m\":1,"
        "\"below_surface_m\":0,\"envelope\":null,\"free_count\":1,"
        "\"occupied_count\":1,\"saturated\":true,\"created_at_ms\":1,"
        "\"registration\":{\"frame\":\"project_z_up\",\"source\":\"test\","
        "\"geometry_computed_at_ms\":123456,\"sensor_to_scan\":[1,0,0,0,"
        "0,1,0,0,0,0,1,0,0,0,0,1],\"residual_m\":0,\"threshold_m\":0.05}}";
    static const int16_t cells[][3] = {
        { 0, 0, 0 },
        { 2, 0, 0 },
    };
    const size_t header_length = sizeof(header) - 1U;
    const size_t payload_length = 6U * (sizeof(cells) / sizeof(cells[0]));
    const size_t length = 20U + header_length + payload_length;
    uint8_t *data = calloc(length, sizeof(*data));
    assert(data != NULL);

    memcpy(data, "GOCCUPY1", 8U);
    write_u32(data, 8U, (uint32_t)header_length);
    write_u32(data, 12U, (uint32_t)payload_length);
    memcpy(data + 20U, header, header_length);
    size_t offset = 20U + header_length;
    for (size_t cell = 0U; cell < sizeof(cells) / sizeof(cells[0]); ++cell) {
        for (size_t axis = 0U; axis < 3U; ++axis) {
            write_u16(data, offset, (uint16_t)cells[cell][axis]);
            offset += 2U;
        }
    }
    write_u32(data, 16U, test_crc32(data + 20U, length - 20U));
    *length_out = length;
    return data;
}

static void test_saturated_artifact_preserves_listed_evidence(void)
{
    size_t length = 0U;
    uint8_t *data = make_saturated_artifact(&length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    free(data);

    assert(occupancy.saturated);
    assert(occupancy.free_count == 1U && occupancy.occupied_count == 1U);
    assert(scan_occupancy_registration_usable(&occupancy, 123456U));

    const double omitted[3] = { 1.5, 0.5, 0.5 };
    const double listed_free[3] = { 0.5, 0.5, 0.5 };
    const double listed_occupied[3] = { 2.5, 0.5, 0.5 };
    assert(scan_occupancy_classify(&occupancy, omitted) == SCAN_OCCUPANCY_UNKNOWN);
    assert(scan_occupancy_classify(&occupancy, listed_free) == SCAN_OCCUPANCY_FREE);
    assert(scan_occupancy_classify(&occupancy, listed_occupied) == SCAN_OCCUPANCY_OCCUPIED);

    const localization_point3d_t omitted_candidate = { 1.5, 0.5, 0.5 };
    const localization_point3d_t free_candidate = { 0.5, 0.5, 0.5 };
    assert(localization_calculate_scan_candidate_allowed(&omitted_candidate, &occupancy));
    assert(!localization_calculate_scan_candidate_allowed(&free_candidate, &occupancy));
    scan_occupancy_free(&occupancy);
}

static uint8_t *read_vector(size_t *length_out)
{
    static const char *const paths[] = {
        "tests/vectors/scan_occupancy_continuation_v2.bin",
        "../../../tests/vectors/scan_occupancy_continuation_v2.bin",
        "../../../../tests/vectors/scan_occupancy_continuation_v2.bin",
    };
    FILE *file = NULL;
    for (size_t index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        file = fopen(paths[index], "rb");
        if (file != NULL) {
            break;
        }
    }
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size > 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)size);
    assert(data != NULL);
    assert(fread(data, 1U, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    *length_out = (size_t)size;
    return data;
}

static uint8_t *read_project_vector(size_t *length_out)
{
    static const char *const paths[] = {
        "tests/vectors/scan_occupancy_v2.bin",
        "../../../tests/vectors/scan_occupancy_v2.bin",
        "../../../../tests/vectors/scan_occupancy_v2.bin",
    };
    FILE *file = NULL;
    for (size_t index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        file = fopen(paths[index], "rb");
        if (file != NULL) break;
    }
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size > 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)size);
    assert(data != NULL);
    assert(fread(data, 1U, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    *length_out = (size_t)size;
    return data;
}

static void test_occupied_candidates_survive_project_frame_decode(void)
{
    size_t length = 0U;
    uint8_t *data = read_project_vector(&length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    free(data);

    assert(occupancy.registration.present);
    assert(scan_occupancy_registration_usable(
        &occupancy, occupancy.registration.geometry_computed_at_ms));
    const double scan_target[3] = { 1.0, 2.0, 3.0 };
    const double *matrix = occupancy.registration.sensor_to_scan;
    const double relative[3] = {
        scan_target[0] - matrix[3],
        scan_target[1] - matrix[7],
        scan_target[2] - matrix[11],
    };
    const double sensor[3] = {
        matrix[0] * relative[0] + matrix[4] * relative[1] + matrix[8] * relative[2],
        matrix[1] * relative[0] + matrix[5] * relative[1] + matrix[9] * relative[2],
        matrix[2] * relative[0] + matrix[6] * relative[1] + matrix[10] * relative[2],
    };
    double transformed[3];
    scan_occupancy_sensor_to_scan(&occupancy, sensor, transformed);
    for (size_t axis = 0U; axis < 3U; ++axis) {
        assert(fabs(transformed[axis] - scan_target[axis]) < 1e-6);
    }
    assert(scan_occupancy_classify(&occupancy, transformed) == SCAN_OCCUPANCY_OCCUPIED);

    const localization_point3d_t candidate = { sensor[0], sensor[1], sensor[2] };
    assert(localization_calculate_scan_candidate_allowed(&candidate, &occupancy));
    scan_occupancy_free(&occupancy);
}

/* Issue #32: an occupancy whose voxel size slipped past the parser must not make the ray sampler spin or run unbounded. */
static void test_absurd_voxel_size_ray_returns_without_looping(void)
{
    static const double sizes[] = { 5e-324, 1e-50, 1e-300, 1e-9 };
    const localization_point3d_t sensor = { 0.0, 0.0, 0.0 };
    const localization_point3d_t far_candidate = { 1.0, 0.0, 0.0 };
    const localization_point3d_t same_point = { 0.0, 0.0, 0.0 };
    for (size_t index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        scan_occupancy_t occupancy;
        memset(&occupancy, 0, sizeof(occupancy));
        occupancy.voxel_size_m = sizes[index];
        const localization_calculate_scan_ray_context_t ray = { .occupancy = &occupancy, .sensor_clearance_m = 0.0 };
        assert(localization_calculate_scan_ray_clear(&sensor, &far_candidate, (void *)&ray));
        assert(localization_calculate_scan_ray_clear(&sensor, &same_point, (void *)&ray));
        const localization_calculate_scan_ray_context_t past_end = { .occupancy = &occupancy, .sensor_clearance_m = 5.0 };
        assert(localization_calculate_scan_ray_clear(&sensor, &far_candidate, (void *)&past_end));
    }
}

int main(void)
{
    test_absurd_voxel_size_ray_returns_without_looping();
    test_saturated_artifact_preserves_listed_evidence();
    size_t length = 0U;
    uint8_t *data = read_vector(&length);
    scan_occupancy_t occupancy;
    assert(scan_occupancy_parse(data, length, &occupancy) == ESP_OK);
    free(data);

    assert(occupancy.voxel_size_m == 0.1);
    assert(occupancy.free_count == 4U);
    const double candidate_scan[3] = { 0.05, 0.05, 0.05 };
    const localization_point3d_t candidate = { 0.05, 0.05, 0.05 };
    static const double fine_cells[4][3] = {
        { 0.05, 0.05, 0.05 },
        { 0.15, 0.05, 0.05 },
        { 0.05, 0.35, 0.05 },
        { 0.25, 0.35, 0.05 },
    };
    for (size_t cell = 0U; cell < 4U; ++cell) {
        const localization_point3d_t fine_candidate = { fine_cells[cell][0], fine_cells[cell][1], fine_cells[cell][2] };
        assert(scan_occupancy_classify(&occupancy, fine_cells[cell]) == SCAN_OCCUPANCY_FREE);
        assert(!localization_calculate_scan_candidate_allowed(&fine_candidate, &occupancy));
    }
    assert(scan_occupancy_classify(&occupancy, candidate_scan) == SCAN_OCCUPANCY_FREE);
    assert(localization_calculate_scan_suea_cell(&candidate, &occupancy) == LOCALIZATION_SUEA_CELL_EMPTY);

    const localization_calculate_scan_ray_context_t ray = {
        .occupancy = &occupancy,
        .sensor_clearance_m = LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * occupancy.voxel_size_m,
    };
    const localization_point3d_t sensor = { 0.55, 0.05, 0.05 };
    assert(!localization_calculate_scan_ray_clear(&sensor, &candidate, (void *)&ray));

    const localization_calculate_scan_velocity_context_t velocity = {
        .occupancy = &occupancy,
        .velocity_mps = 300.0,
        .sample_radius_m = 0.0,
        .planar = true,
    };
    assert(localization_calculate_scan_eikonal_velocity(&candidate, (void *)&velocity) == 0.0);

    scan_occupancy_t coarse_dedup = occupancy;
    const double coarse_survivor[3] = { 0.25, 0.35, 0.05 };
    bool found_coarse_survivor = false;
    for (size_t index = 0U; index < occupancy.free_count; ++index) {
        coarse_dedup.free_keys = &occupancy.free_keys[index];
        coarse_dedup.free_count = 1U;
        if (scan_occupancy_classify(&coarse_dedup, coarse_survivor) == SCAN_OCCUPANCY_FREE) {
            found_coarse_survivor = true;
            break;
        }
    }
    assert(found_coarse_survivor);
    assert(scan_occupancy_classify(&coarse_dedup, candidate_scan) == SCAN_OCCUPANCY_UNKNOWN);
    assert(localization_calculate_scan_candidate_allowed(&candidate, &coarse_dedup));
    assert(localization_calculate_scan_suea_cell(&candidate, &coarse_dedup) == LOCALIZATION_SUEA_CELL_UNKNOWN);
    const localization_calculate_scan_ray_context_t coarse_ray = {
        .occupancy = &coarse_dedup,
        .sensor_clearance_m = LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * coarse_dedup.voxel_size_m,
    };
    assert(localization_calculate_scan_ray_clear(&sensor, &candidate, (void *)&coarse_ray));
    const localization_calculate_scan_velocity_context_t coarse_velocity = {
        .occupancy = &coarse_dedup,
        .velocity_mps = 300.0,
        .sample_radius_m = 0.0,
        .planar = true,
    };
    assert(localization_calculate_scan_eikonal_velocity(&candidate, (void *)&coarse_velocity) == 300.0);

    scan_occupancy_free(&occupancy);
    test_occupied_candidates_survive_project_frame_decode();
    return 0;
}
