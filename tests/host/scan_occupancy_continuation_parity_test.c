#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "localization_calculate_policy.h"
#include "scan_occupancy.h"

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

int main(void)
{
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
    return 0;
}
