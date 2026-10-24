#undef malloc
#undef calloc
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "localization_eikonal.h"
#include "localization_suea.h"

static size_t s_allocations;
static size_t s_requested_bytes;

void *localization_grid_contract_malloc(size_t size)
{
    assert(size > 0U);
    ++s_allocations;
    s_requested_bytes = size;
    return NULL;
}

void *localization_grid_contract_calloc(size_t count, size_t size)
{
    assert(count > 0U && size > 0U && count <= SIZE_MAX / size);
    return localization_grid_contract_malloc(count * size);
}

#if defined(GRID_CONTRACT_EIKONAL)
typedef localization_eikonal_model_t test_grid_t;
static esp_err_t init_grid(test_grid_t *grid, const localization_point3d_t *lo,
    const localization_point3d_t *hi, double cell, size_t limit)
{
    return localization_eikonal_model_init(grid, lo, hi, cell, limit, 300.0);
}
static bool grid_empty(const test_grid_t *grid) { return grid->slowness_s_per_m == NULL; }
#else
typedef localization_suea_grid_t test_grid_t;
static esp_err_t init_grid(test_grid_t *grid, const localization_point3d_t *lo,
    const localization_point3d_t *hi, double cell, size_t limit)
{
    return localization_suea_grid_init(grid, lo, hi, cell, limit);
}
static bool grid_empty(const test_grid_t *grid) { return grid->cells == NULL; }
#endif

int main(void)
{
    const localization_point3d_t lo = {0.0, 0.0, 0.0};
    const localization_point3d_t hi = {1.0, 1.0, 1.0};
    test_grid_t grid;
    assert(init_grid(&grid, &lo, &hi, DBL_MIN, 0U) == ESP_ERR_INVALID_SIZE);
    assert(s_allocations == 0U && grid_empty(&grid));
    assert(grid.nx == 0U && grid.ny == 0U && grid.nz == 0U);

    const localization_point3d_t far = {DBL_MAX, 0.0, 0.0};
    assert(init_grid(&grid, &lo, &far, DBL_MAX / 2.0, 1U) == ESP_ERR_INVALID_SIZE);
    assert(s_allocations == 0U && grid_empty(&grid));

    /* Zero extent remains a one-cell model even with a tiny cell size. */
    assert(init_grid(&grid, &lo, &lo, DBL_MIN, 0U) == ESP_ERR_NO_MEM);
    assert(s_allocations == 1U && s_requested_bytes > 0U && grid_empty(&grid));

    /* Values around SIZE_MAX are rounded by double on 64-bit hosts. */
    const unsigned bits = (unsigned)(sizeof(size_t) * CHAR_BIT);
    const double side_nodes = ldexp(1.0, (int)(bits / 2U));
    const localization_point3d_t large = {side_nodes - 1.0, side_nodes - 1.0, 0.0};
    s_allocations = 0U;
    assert(init_grid(&grid, &lo, &large, 1.0, SIZE_MAX) == ESP_ERR_NO_MEM);
    assert(s_allocations == 1U && grid_empty(&grid));
    const size_t minimum_side = (size_t)(side_nodes / 4.0);
#if defined(GRID_CONTRACT_EIKONAL)
    assert(s_requested_bytes >= minimum_side * minimum_side * sizeof(float));
    const localization_point3d_t byte_overflow = {ldexp(1.0, (int)bits - 2) - 1.0, 0.0, 0.0};
    s_allocations = 0U;
    assert(init_grid(&grid, &lo, &byte_overflow, 1.0, SIZE_MAX) == ESP_ERR_NO_MEM);
    assert(s_allocations == 1U && s_requested_bytes > SIZE_MAX / 4U && grid_empty(&grid));
#else
    assert(s_requested_bytes >= minimum_side * minimum_side);
#endif
    puts("grid allocation bounds: no invalid-size allocations or wrapped requests");
    return 0;
}
