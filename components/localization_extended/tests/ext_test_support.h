#pragma once

#include <math.h>
#include <stdint.h>

#include "localization.h"
#include "localization_extended.h"

/* Synthetic ground truth shared by the localization_extended tests. */

static inline double ext_test_distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* Deterministic uniform noise in [-amplitude, +amplitude]. */
static inline double ext_test_uniform(uint32_t *state, double amplitude)
{
    *state = *state * 1664525U + 1013904223U;
    const double unit = (double)(*state >> 8) / (double)(1U << 24);
    return (2.0 * unit - 1.0) * amplitude;
}

/* Deterministic standard normal (Box-Muller). */
static inline double ext_test_normal(uint32_t *state)
{
    double u1 = 0.5 * (ext_test_uniform(state, 1.0) + 1.0);
    const double u2 = 0.5 * (ext_test_uniform(state, 1.0) + 1.0);
    if (u1 < 1.0e-12) {
        u1 = 1.0e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* A depth-resolving layout: sensors on a rubble surface with relief. */
static const localization_point3d_t EXT_TEST_SENSORS_3D[8] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.3 },
    { 0.0, 3.5, 0.6 },
    { 4.0, 3.5, -0.2 },
    { 2.0, -0.5, 0.8 },
    { -0.5, 2.0, -0.4 },
    { 2.2, 4.0, 0.1 },
    { 4.5, 1.8, 0.5 },
};

/* The current hardware: four pods flat on the surface. */
static const localization_point3d_t EXT_TEST_SENSORS_FLAT[4] = {
    { 0.0, 0.0, 0.0 },
    { 3.0, 0.0, 0.0 },
    { 0.0, 2.5, 0.0 },
    { 3.0, 2.5, 0.0 },
};

static inline localization_extended_bounds_t ext_test_bounds_3d(void)
{
    const localization_extended_bounds_t bounds = {
        { -1.0, -1.0, -3.0 }, { 5.0, 4.5, 0.8 }, 0.1,
    };
    return bounds;
}

static inline void ext_test_observations(
    const localization_point3d_t *sensors,
    size_t count,
    localization_point3d_t source,
    double emission_s,
    double velocity_mps,
    double noise_sigma_us,
    uint32_t *state,
    localization_observation_t *out
)
{
    for (size_t i = 0U; i < count; ++i) {
        const double noise = noise_sigma_us > 0.0 ? noise_sigma_us * ext_test_normal(state) : 0.0;
        out[i].sensor_position = sensors[i];
        out[i].arrival_time_us = (int64_t)llround(
            (emission_s + ext_test_distance(source, sensors[i]) / velocity_mps) * 1.0e6 + noise
        );
    }
}
