#include <inttypes.h>
#include <stdlib.h>

/* Reuse the MAX-132 oracle's sensor fixture and exact integer-us generator. */
#define main max132_oracle_reference_main
#include "max132_tdoa_oracle_test.c"
#undef main

#define RANDOM_CASES 2000U
#define RANDOM_SEED UINT32_C(0x4d415831)

/* Match the oracle's tolerances: 15 mm per coordinate covers its sub-mm
 * grid, <= 0.5 us pick rounding, and modest geometry conditioning; 10 us
 * allows that position uncertainty plus integer-us emission rounding. */
#define POSITION_TOLERANCE_M 0.015
#define EMISSION_TOLERANCE_US 10.0

static uint32_t g_random_state;
static uint32_t g_random_case;

static void fail_random_case(const char *stage, const char *check)
{
    fprintf(stderr,
        "FAIL seed=0x%08" PRIx32 " case=%" PRIu32
        " state=0x%08" PRIx32 " stage=%s check=%s\n",
        RANDOM_SEED, g_random_case, g_random_state, stage, check);
    fflush(stderr);
}

#define RANDOM_ASSERT(condition, stage) \
    do { \
        if (!(condition)) { \
            fail_random_case((stage), #condition); \
            assert(condition); \
            abort(); \
        } \
    } while (0)

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static double random_between(uint32_t *state, double low, double high)
{
    const double unit = (double)next_random(state) / (double)UINT32_MAX;
    return low + (high - low) * unit;
}

static localization_config_t invariant_config(double speed_mps)
{
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = speed_mps;
    config.max_iterations = 100U;
    config.convergence_m = 1.0e-8;
    return config;
}

static void assert_random_result(
    esp_err_t status,
    const localization_result_t *result,
    const localization_config_t *config,
    const char *stage
)
{
    RANDOM_ASSERT(status == ESP_OK, stage);
    RANDOM_ASSERT(result->converged, stage);
    RANDOM_ASSERT(result->iterations > 0U, stage);
    RANDOM_ASSERT(result->iterations <= config->max_iterations, stage);
    RANDOM_ASSERT(point_is_finite(result->source_position), stage);
    RANDOM_ASSERT(isfinite(result->rms_residual_us), stage);
}

static void assert_position_close(
    localization_point3d_t actual,
    localization_point3d_t expected,
    const char *stage
)
{
    RANDOM_ASSERT(
        fabs(actual.x_m - expected.x_m) <= POSITION_TOLERANCE_M, stage);
    RANDOM_ASSERT(
        fabs(actual.y_m - expected.y_m) <= POSITION_TOLERANCE_M, stage);
    RANDOM_ASSERT(
        fabs(actual.z_m - expected.z_m) <= POSITION_TOLERANCE_M, stage);
}

static void make_random_case(
    uint32_t *state,
    localization_observation_t *observations,
    localization_point3d_t *truth,
    double *speed_mps,
    int64_t *emission_time_us
)
{
    const size_t sensor_count = ARRAY_COUNT(g_validation_sensors);
    localization_point3d_t center = { 0.0, 0.0, 0.0 };
    for (size_t i = 0U; i < sensor_count; ++i) {
        center.x_m += g_validation_sensors[i].x_m / (double)sensor_count;
        center.y_m += g_validation_sensors[i].y_m / (double)sensor_count;
        center.z_m += g_validation_sensors[i].z_m / (double)sensor_count;
    }

    /* Perturb the already validated 8-sensor oracle array: scale and yaw
     * preserve its depth-resolving shape; small independent jitter varies it. */
    const double scale = random_between(state, 0.94, 1.06);
    const double angle = random_between(state, -3.141592653589793, 3.141592653589793);
    const double cosine = cos(angle);
    const double sine = sin(angle);
    const double translate_x = random_between(state, -0.35, 0.35);
    const double translate_y = random_between(state, -0.35, 0.35);
    localization_point3d_t transformed[LOCALIZATION_MAX_SENSORS];
    localization_point3d_t transformed_center = { 0.0, 0.0, 0.0 };

    for (size_t i = 0U; i < sensor_count; ++i) {
        const double dx = g_validation_sensors[i].x_m - center.x_m;
        const double dy = g_validation_sensors[i].y_m - center.y_m;
        transformed[i] = (localization_point3d_t) {
            .x_m = center.x_m + scale * (cosine * dx - sine * dy) +
                translate_x + random_between(state, -0.04, 0.04),
            .y_m = center.y_m + scale * (sine * dx + cosine * dy) +
                translate_y + random_between(state, -0.04, 0.04),
            .z_m = center.z_m + scale *
                (g_validation_sensors[i].z_m - center.z_m) +
                random_between(state, -0.04, 0.04),
        };
        transformed_center.x_m += transformed[i].x_m / (double)sensor_count;
        transformed_center.y_m += transformed[i].y_m / (double)sensor_count;
        transformed_center.z_m += transformed[i].z_m / (double)sensor_count;
    }

    *truth = (localization_point3d_t) {
        .x_m = transformed_center.x_m + random_between(state, -0.55, 0.55),
        .y_m = transformed_center.y_m + random_between(state, -0.55, 0.55),
        .z_m = transformed_center.z_m + random_between(state, -0.55, 0.55),
    };
    *speed_mps = random_between(state, 1650.0, 1850.0);
    *emission_time_us = INT64_C(7000000) +
        (int64_t)(next_random(state) % UINT32_C(3000000));

    for (size_t i = 0U; i < sensor_count; ++i) {
        /* These are test-domain limits around the existing oracle fixture;
         * localization_tdoa_solve itself declares no coordinate bounds. */
        RANDOM_ASSERT(fabs(transformed[i].x_m) <= 5.5, "generated bounds");
        RANDOM_ASSERT(fabs(transformed[i].y_m) <= 5.5, "generated bounds");
        RANDOM_ASSERT(fabs(transformed[i].z_m) <= 5.5, "generated bounds");
    }
    RANDOM_ASSERT(fabs(truth->x_m) <= 2.0, "generated bounds");
    RANDOM_ASSERT(fabs(truth->y_m) <= 2.0, "generated bounds");
    RANDOM_ASSERT(fabs(truth->z_m) <= 2.0, "generated bounds");

    make_observations(
        transformed,
        sensor_count,
        *truth,
        *speed_mps,
        *emission_time_us,
        observations
    );
}

static void test_randomized_invariants(void)
{
    g_random_state = RANDOM_SEED;
    g_random_case = 0U;

    for (uint32_t test_case = 1U; test_case <= RANDOM_CASES; ++test_case) {
        g_random_case = test_case;
        localization_observation_t observations[LOCALIZATION_MAX_SENSORS];
        localization_observation_t permuted[LOCALIZATION_MAX_SENSORS];
        localization_observation_t shifted[LOCALIZATION_MAX_SENSORS];
        localization_point3d_t truth;
        double speed_mps;
        int64_t emission_time_us;
        make_random_case(&g_random_state, observations, &truth, &speed_mps,
            &emission_time_us);

        const size_t sensor_count = ARRAY_COUNT(g_validation_sensors);
        const localization_config_t config = invariant_config(speed_mps);
        localization_result_t baseline;
        memset(&baseline, 0, sizeof(baseline));
        const esp_err_t baseline_status = localization_tdoa_solve(
            observations, sensor_count, &config, NULL, &baseline);
        assert_random_result(baseline_status, &baseline, &config, "baseline");
        assert_position_close(baseline.source_position, truth, "truth");
        /* The oracle's same integer-us synthetic picks have <= about 1 us
         * differential RMS; 1.5 us retains its small numerical margin. */
        RANDOM_ASSERT(baseline.rms_residual_us <= 1.5, "quantized residual");
        const double independent_rms = tdoa_rms_at(
            observations, sensor_count, baseline.source_position, speed_mps);
        RANDOM_ASSERT(isfinite(independent_rms), "oracle residual finite");
        /* Match the oracle's 1e-5 us band for two independent residual
         * recomputations; it covers floating-point operation-order noise. */
        RANDOM_ASSERT(fabs(baseline.rms_residual_us - independent_rms) <= 1.0e-5,
            "oracle residual agreement");
        RANDOM_ASSERT(fabs((double)baseline.emission_time_us -
            (double)emission_time_us) <= EMISSION_TOLERANCE_US,
            "truth emission time");

        memcpy(permuted, observations, sizeof(observations));
        for (size_t i = sensor_count - 1U; i > 0U; --i) {
            const size_t j = (size_t)(next_random(&g_random_state) %
                (uint32_t)(i + 1U));
            const localization_observation_t swap = permuted[i];
            permuted[i] = permuted[j];
            permuted[j] = swap;
        }
        localization_result_t permuted_result;
        memset(&permuted_result, 0, sizeof(permuted_result));
        const esp_err_t permuted_status = localization_tdoa_solve(
            permuted, sensor_count, &config, NULL, &permuted_result);
        assert_random_result(permuted_status, &permuted_result, &config,
            "sensor permutation");
        /* The solver uses observations[0] as its differential reference.
         * Changing that reference may change floating-point rounding, but
         * must leave each physical coordinate within the oracle's 15 mm. */
        assert_position_close(permuted_result.source_position,
            baseline.source_position, "sensor permutation position");

        memcpy(shifted, observations, sizeof(observations));
        const int64_t common_offset_us = INT64_C(1000000) +
            (int64_t)(next_random(&g_random_state) % UINT32_C(1000000));
        for (size_t i = 0U; i < sensor_count; ++i) {
            shifted[i].arrival_time_us += common_offset_us;
        }
        localization_result_t shifted_result;
        memset(&shifted_result, 0, sizeof(shifted_result));
        const esp_err_t shifted_status = localization_tdoa_solve(
            shifted, sensor_count, &config, NULL, &shifted_result);
        assert_random_result(shifted_status, &shifted_result, &config,
            "common timestamp offset");
        assert_position_close(shifted_result.source_position,
            baseline.source_position, "timestamp offset position");
        const double emission_delta_us = (double)(
            shifted_result.emission_time_us - baseline.emission_time_us);
        RANDOM_ASSERT(fabs(emission_delta_us - (double)common_offset_us) <=
            EMISSION_TOLERANCE_US, "timestamp offset emission");
    }

    printf("PASS randomized cases=%u seed=0x%08" PRIx32
        " solver_runs=%u\n", RANDOM_CASES, RANDOM_SEED,
        RANDOM_CASES * 3U);
}

/*
 * J3: rigid translation/rotation invariance. Only a rotation about the
 * vertical axis plus an XY translation is applied. The solver and its search
 * bounds treat z as depth (the geometry validation checks depth spread and the
 * result is bounded in depth), so a general 3D rotation or a vertical
 * translation changes which physical points are inside the valid volume and is
 * not an invariance this solver promises. Within the horizontal plane the
 * arrival times depend only on distances, which a rotation about z and an XY
 * translation preserve exactly, so the solved position mapped back must equal
 * the original within the oracle tolerance.
 */
static void test_vertical_axis_rigid_transform(void)
{
    const size_t sensor_count = ARRAY_COUNT(g_validation_sensors);
    const localization_point3d_t truth = { 0.45, 0.36, 0.52 };
    const double speed_mps = 1700.0;
    const int64_t emission_time_us = INT64_C(1234567);
    const double angle = 0.73;
    const double cosine = cos(angle);
    const double sine = sin(angle);
    const double translate_x = 0.8;
    const double translate_y = -0.6;
    localization_observation_t original[LOCALIZATION_MAX_SENSORS];
    localization_observation_t transformed[LOCALIZATION_MAX_SENSORS];
    localization_point3d_t transformed_sensors[LOCALIZATION_MAX_SENSORS];

    make_observations(g_validation_sensors, sensor_count, truth, speed_mps,
        emission_time_us, original);
    const localization_point3d_t transformed_truth = {
        .x_m = cosine * truth.x_m - sine * truth.y_m + translate_x,
        .y_m = sine * truth.x_m + cosine * truth.y_m + translate_y,
        .z_m = truth.z_m,
    };
    for (size_t i = 0U; i < sensor_count; ++i) {
        transformed_sensors[i] = (localization_point3d_t) {
            .x_m = cosine * g_validation_sensors[i].x_m -
                sine * g_validation_sensors[i].y_m + translate_x,
            .y_m = sine * g_validation_sensors[i].x_m +
                cosine * g_validation_sensors[i].y_m + translate_y,
            .z_m = g_validation_sensors[i].z_m,
        };
    }
    make_observations(transformed_sensors, sensor_count, transformed_truth,
        speed_mps, emission_time_us, transformed);

    const localization_config_t config = invariant_config(speed_mps);
    localization_result_t base_result;
    localization_result_t transformed_result;
    memset(&base_result, 0, sizeof(base_result));
    memset(&transformed_result, 0, sizeof(transformed_result));
    assert(localization_tdoa_solve(original, sensor_count, &config, NULL,
        &base_result) == ESP_OK);
    assert(localization_tdoa_solve(transformed, sensor_count, &config, NULL,
        &transformed_result) == ESP_OK);
    assert(base_result.converged && transformed_result.converged);
    assert(base_result.iterations > 0U &&
        base_result.iterations <= config.max_iterations);
    assert(transformed_result.iterations > 0U &&
        transformed_result.iterations <= config.max_iterations);
    assert(point_is_finite(base_result.source_position));
    assert(point_is_finite(transformed_result.source_position));

    /* Invert the tested yaw and XY translation before comparing frames. A
     * general 3-D rotation also preserves this isotropic distance equation,
     * but tilting the calibrated vertical axis is not a valid deployment
     * frame transform for depth-aware use, so only yaw is tested here. */
    const double x = transformed_result.source_position.x_m - translate_x;
    const double y = transformed_result.source_position.y_m - translate_y;
    const localization_point3d_t round_trip = {
        .x_m = cosine * x + sine * y,
        .y_m = -sine * x + cosine * y,
        .z_m = transformed_result.source_position.z_m,
    };
    assert_near(round_trip.x_m, base_result.source_position.x_m,
        POSITION_TOLERANCE_M);
    assert_near(round_trip.y_m, base_result.source_position.y_m,
        POSITION_TOLERANCE_M);
    assert_near(round_trip.z_m, base_result.source_position.z_m,
        POSITION_TOLERANCE_M);
    puts("PASS vertical-axis rigid transform");
}

int main(void)
{
    /* The host test harness exposes no allocation counter, so heap leak
     * checking is not possible here; the solver's bounded-loop/output checks
     * run for all 2,000 cases (6,000 solver calls). */
    test_randomized_invariants();
    test_vertical_axis_rigid_transform();
    puts("MAX-132 solver invariants passed");
    return 0;
}
