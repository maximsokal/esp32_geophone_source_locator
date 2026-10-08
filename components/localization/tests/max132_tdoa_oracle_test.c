#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "localization.h"

#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

typedef struct
{
    const char *name;
    const localization_point3d_t *sensors;
    size_t sensor_count;
    localization_point3d_t source;
    double speed_mps;
    int64_t emission_time_us;
} oracle_fixture_t;

typedef struct
{
    localization_point3d_t source;
    double rms_residual_us;
} oracle_result_t;

typedef struct
{
    localization_observation_t observations[LOCALIZATION_MAX_SENSORS + 1U];
    size_t count;
    localization_point3d_t truth;
    double speed_mps;
    int64_t emission_time_us;
    const localization_observation_t *observations_arg;
    const localization_config_t *config_arg;
    const localization_point3d_t *guess_arg;
    localization_result_t result;
    localization_result_t *result_arg;
    localization_config_t config;
} tdoa_case_state_t;

typedef void (*tdoa_case_setup_fn)(tdoa_case_state_t *state);

typedef struct
{
    const char *name;
    const char *expected_reason;
    esp_err_t expected_status;
    tdoa_case_setup_fn setup;
    bool known_defect;
} tdoa_case_t;

static const localization_point3d_t g_sensors_2d_source[] = {
    { 0.0, 0.0, 0.0 }, { 4.0, 0.0, 0.0 }, { 0.0, 3.0, 0.0 },
    { 0.0, 0.0, 4.0 }, { 3.5, 2.0, 1.7 },
};

static const localization_point3d_t g_sensors_origin[] = {
    { 2.1, 0.3, -0.4 }, { -1.4, 2.7, 0.2 },
    { 0.3, -1.8, 2.9 }, { -2.5, -0.9, -2.2 },
};

static const localization_point3d_t g_sensors_eight[] = {
    { 3.4, 0.2, 0.7 }, { -2.1, 3.2, 0.4 },
    { -0.3, -2.7, 3.3 }, { -2.2, -1.3, -2.8 },
    { 1.8, 2.0, 2.5 }, { 3.0, -2.2, -0.5 },
    { -3.2, 0.7, 1.1 }, { 0.9, 3.5, -2.4 },
};

static const localization_point3d_t g_validation_sensors[] = {
    { 0.0, 0.0, 0.0 }, { 4.0, 0.0, 0.0 },
    { 0.0, 3.0, 0.0 }, { 0.0, 0.0, 3.0 },
    { 3.2, 2.1, 1.8 }, { -1.5, 2.4, -0.7 },
    { 2.8, -2.1, 0.6 }, { -2.3, -1.2, 2.7 },
};

static double distance_m(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

static bool point_is_finite(localization_point3d_t point)
{
    return isfinite(point.x_m) && isfinite(point.y_m) && isfinite(point.z_m);
}

static void make_observations(
    const localization_point3d_t *sensors,
    size_t sensor_count,
    localization_point3d_t source,
    double speed_mps,
    int64_t emission_time_us,
    localization_observation_t *observations
)
{
    assert(sensor_count <= LOCALIZATION_MAX_SENSORS);
    for (size_t i = 0U; i < sensor_count; ++i) {
        const double travel_time_us =
            distance_m(source, sensors[i]) / speed_mps * 1.0e6;
        assert(isfinite(travel_time_us));
        assert(travel_time_us >= 0.0);
        assert(travel_time_us < 1.0e9);
        observations[i].sensor_position = sensors[i];
        /* Explicit convention: absolute arrival = integer emission epoch +
         * rounded (distance[m] / speed[m/s]) converted from seconds to us. */
        observations[i].arrival_time_us = emission_time_us + (int64_t)llround(travel_time_us);
    }
}

static double tdoa_rms_at(
    const localization_observation_t *observations,
    size_t count,
    localization_point3d_t candidate,
    double speed_mps
)
{
    double squared_sum_us = 0.0;
    const double reference_arrival_us = (double)observations[0].arrival_time_us;
    const double reference_distance_m = distance_m(
        candidate, observations[0].sensor_position
    );

    for (size_t i = 1U; i < count; ++i) {
        const double observed_delta_us =
            (double)observations[i].arrival_time_us - reference_arrival_us;
        const double predicted_delta_us =
            (distance_m(candidate, observations[i].sensor_position) - reference_distance_m) /
            speed_mps * 1.0e6;
        const double residual_us = predicted_delta_us - observed_delta_us;
        squared_sum_us += residual_us * residual_us;
    }
    return sqrt(squared_sum_us / (double)(count - 1U));
}

/* Independent reference: search the observed differential-time residual on
 * an inclusive Cartesian grid.  It does not use Gauss-Newton, its Jacobian,
 * or production solver output.  Refining 0.5 m by decades to 0.5 mm gives a
 * final half-cell error <= 0.44 mm before integer-us arrival quantization. */
static oracle_result_t coarse_to_fine_oracle(
    const localization_observation_t *observations,
    size_t count,
    double speed_mps
)
{
    localization_point3d_t center = { 0.0, 0.0, 0.0 };
    for (size_t i = 0U; i < count; ++i) {
        center.x_m += observations[i].sensor_position.x_m / (double)count;
        center.y_m += observations[i].sensor_position.y_m / (double)count;
        center.z_m += observations[i].sensor_position.z_m / (double)count;
    }

    oracle_result_t best = { .rms_residual_us = INFINITY };
    double radius_m = 6.0;
    double step_m = 0.5;

    for (size_t level = 0U; level < 4U; ++level) {
        const size_t divisions = level == 0U ? 24U : 20U;
        step_m = (2.0 * radius_m) / (double)divisions;
        const double low_x = center.x_m - radius_m;
        const double low_y = center.y_m - radius_m;
        const double low_z = center.z_m - radius_m;
        const double high_x = center.x_m + radius_m;
        const double high_y = center.y_m + radius_m;
        const double high_z = center.z_m + radius_m;

        best.rms_residual_us = INFINITY;
        for (size_t ix = 0U; ix <= divisions; ++ix) {
            const double x = ix == divisions ? high_x : low_x + step_m * (double)ix;
            for (size_t iy = 0U; iy <= divisions; ++iy) {
                const double y = iy == divisions ? high_y : low_y + step_m * (double)iy;
                for (size_t iz = 0U; iz <= divisions; ++iz) {
                    const double z = iz == divisions ? high_z : low_z + step_m * (double)iz;
                    const localization_point3d_t candidate = { x, y, z };
                    /* Both search faces are candidates: equality is included. */
                    assert(x >= low_x && x <= high_x);
                    assert(y >= low_y && y <= high_y);
                    assert(z >= low_z && z <= high_z);
                    const double residual_us = tdoa_rms_at(
                        observations, count, candidate, speed_mps
                    );
                    if (residual_us < best.rms_residual_us) {
                        best.source = candidate;
                        best.rms_residual_us = residual_us;
                    }
                }
            }
        }

        center = best.source;
        radius_m = step_m;
    }
    return best;
}

static void assert_near(double actual, double expected, double tolerance)
{
    assert(isfinite(actual));
    assert(fabs(actual - expected) <= tolerance);
}

static void run_oracle_fixture(const oracle_fixture_t *fixture)
{
    localization_observation_t observations[LOCALIZATION_MAX_SENSORS];
    make_observations(
        fixture->sensors,
        fixture->sensor_count,
        fixture->source,
        fixture->speed_mps,
        fixture->emission_time_us,
        observations
    );

    const oracle_result_t oracle = coarse_to_fine_oracle(
        observations, fixture->sensor_count, fixture->speed_mps
    );
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = fixture->speed_mps;
    config.max_iterations = 100U;
    config.convergence_m = 1.0e-8;

    localization_result_t result;
    memset(&result, 0, sizeof(result));
    assert(localization_tdoa_solve(
        observations, fixture->sensor_count, &config, NULL, &result
    ) == ESP_OK);
    assert(result.converged);
    assert(result.iterations > 0U && result.iterations <= config.max_iterations);

    /* 15 mm covers sub-mm grid quantization, <= 0.5 us rounding per pick,
     * and modest geometry conditioning while still detecting sign/unit/frame
     * mistakes (which move these sources by decimeters or more). */
    assert_near(result.source_position.x_m, fixture->source.x_m, 0.015);
    assert_near(result.source_position.y_m, fixture->source.y_m, 0.015);
    assert_near(result.source_position.z_m, fixture->source.z_m, 0.015);
    assert_near(result.source_position.x_m, oracle.source.x_m, 0.02);
    assert_near(result.source_position.y_m, oracle.source.y_m, 0.02);
    assert_near(result.source_position.z_m, oracle.source.z_m, 0.02);

    const double independently_recomputed_rms = tdoa_rms_at(
        observations, fixture->sensor_count, result.source_position,
        fixture->speed_mps
    );
    assert_near(result.rms_residual_us, independently_recomputed_rms, 1.0e-5);
    /* Differential residual from independently rounded integer-us arrivals
     * is at most about 1 us for this noiseless synthetic data. */
    assert(result.rms_residual_us <= 1.5);

    double emission_sum_us = 0.0;
    for (size_t i = 0U; i < fixture->sensor_count; ++i) {
        emission_sum_us += (double)observations[i].arrival_time_us -
            distance_m(oracle.source, observations[i].sensor_position) /
            fixture->speed_mps * 1.0e6;
    }
    const double oracle_emission_us = emission_sum_us / (double)fixture->sensor_count;
    /* The oracle uses a 0.5 mm final cell and each input pick is rounded to
     * 1 us; 10 us allows that plus solver stopping tolerance. */
    assert_near((double)result.emission_time_us, oracle_emission_us, 10.0);
    assert_near(
        (double)result.emission_time_us,
        (double)fixture->emission_time_us,
        10.0
    );
    printf("PASS oracle %s sensors=%u iterations=%u rms_us=%.6f\n",
        fixture->name, (unsigned)fixture->sensor_count,
        (unsigned)result.iterations, result.rms_residual_us);
}

static void setup_base(tdoa_case_state_t *state)
{
    state->count = 5U;
    state->truth = (localization_point3d_t) { 0.45, 0.36, 0.52 };
    state->speed_mps = 1700.0;
    state->emission_time_us = 1234567;
    make_observations(g_validation_sensors, state->count, state->truth,
        state->speed_mps, state->emission_time_us, state->observations);
    state->observations_arg = state->observations;
    state->config = localization_default_config();
    state->config.propagation_speed_mps = state->speed_mps;
    state->config.max_iterations = 100U;
    state->config.convergence_m = 1.0e-8;
    state->config_arg = &state->config;
    state->guess_arg = NULL;
    state->result_arg = &state->result;
}

static void setup_count_zero(tdoa_case_state_t *state) { state->count = 0U; }
static void setup_count_one(tdoa_case_state_t *state) { state->count = 1U; }
static void setup_count_two(tdoa_case_state_t *state) { state->count = 2U; }
static void setup_count_three(tdoa_case_state_t *state) { state->count = 3U; }
static void setup_count_four(tdoa_case_state_t *state) { state->count = 4U; }
static void setup_count_eight(tdoa_case_state_t *state)
{
    state->count = 8U;
    make_observations(g_validation_sensors, state->count, state->truth,
        state->speed_mps, state->emission_time_us, state->observations);
}
static void setup_count_nine(tdoa_case_state_t *state) { state->count = 9U; }
static void setup_null_observations(tdoa_case_state_t *state) { state->observations_arg = NULL; }
static void setup_null_config(tdoa_case_state_t *state) { state->config_arg = NULL; }
static void setup_null_result(tdoa_case_state_t *state) { state->result_arg = NULL; }
static void setup_zero_speed(tdoa_case_state_t *state) { state->config.propagation_speed_mps = 0.0; }
static void setup_negative_speed(tdoa_case_state_t *state) { state->config.propagation_speed_mps = -1.0; }
static void setup_nan_speed(tdoa_case_state_t *state) { state->config.propagation_speed_mps = NAN; }
static void setup_positive_infinite_speed(tdoa_case_state_t *state) { state->config.propagation_speed_mps = INFINITY; }
static void setup_negative_infinite_speed(tdoa_case_state_t *state) { state->config.propagation_speed_mps = -INFINITY; }
static void setup_nan_x(tdoa_case_state_t *state) { state->observations[1].sensor_position.x_m = NAN; }
static void setup_positive_infinite_x(tdoa_case_state_t *state) { state->observations[1].sensor_position.x_m = INFINITY; }
static void setup_negative_infinite_x(tdoa_case_state_t *state) { state->observations[1].sensor_position.x_m = -INFINITY; }
static void setup_nan_guess(tdoa_case_state_t *state)
{
    static const localization_point3d_t bad_guess = { NAN, 0.0, 0.0 };
    state->guess_arg = &bad_guess;
}
static void setup_zero_iterations(tdoa_case_state_t *state) { state->config.max_iterations = 0U; }
static void setup_zero_convergence(tdoa_case_state_t *state) { state->config.convergence_m = 0.0; }
static void setup_negative_damping(tdoa_case_state_t *state) { state->config.damping = -1.0; }

static void setup_all_colocated(tdoa_case_state_t *state)
{
    for (size_t i = 0U; i < state->count; ++i) {
        state->observations[i].sensor_position = (localization_point3d_t) { 1.0, 2.0, 3.0 };
    }
}

static void setup_collinear(tdoa_case_state_t *state)
{
    for (size_t i = 0U; i < state->count; ++i) {
        state->observations[i].sensor_position = (localization_point3d_t) { (double)i, 0.0, 0.0 };
    }
}

static void setup_coplanar(tdoa_case_state_t *state)
{
    for (size_t i = 0U; i < state->count; ++i) {
        state->observations[i].sensor_position.z_m = 0.0;
    }
}

static void setup_depth_threshold_equal(tdoa_case_state_t *state)
{
    const double scale = sqrt(2.0);
    const double threshold_height = 0.05 * scale * scale * scale;
    state->count = 4U;
    state->observations[0].sensor_position = (localization_point3d_t) { 0.0, 0.0, 0.0 };
    state->observations[1].sensor_position = (localization_point3d_t) { 1.0, 0.0, 0.0 };
    state->observations[2].sensor_position = (localization_point3d_t) { 0.0, 1.0, 0.0 };
    state->observations[3].sensor_position = (localization_point3d_t) { 0.0, 0.0, threshold_height };
}

static void setup_depth_just_below_threshold(tdoa_case_state_t *state)
{
    setup_depth_threshold_equal(state);
    state->observations[3].sensor_position.z_m = nextafter(
        state->observations[3].sensor_position.z_m, 0.0
    );
}

static void setup_duplicate_positions(tdoa_case_state_t *state)
{
    state->observations[4] = state->observations[0];
    state->count = 5U;
}

static void setup_stale_result(tdoa_case_state_t *state)
{
    state->config.propagation_speed_mps = 0.0;
    state->result.source_position = (localization_point3d_t) { 8.0, -4.0, 2.0 };
    state->result.emission_time_us = 777777;
    state->result.rms_residual_us = 0.25;
    state->result.iterations = 17U;
    state->result.converged = true;
}

static void setup_high_epoch(tdoa_case_state_t *state)
{
    state->count = 8U;
    state->truth = (localization_point3d_t) { -1.2, 0.85, -1.45 };
    state->speed_mps = 1850.0;
    state->emission_time_us = INT64_MAX - 1000000;
    make_observations(g_sensors_eight, state->count, state->truth,
        state->speed_mps, state->emission_time_us, state->observations);
    state->config.propagation_speed_mps = state->speed_mps;
}

/* The remaining KNOWN_DEFECT row records the unresolved large-epoch precision issue. */
static const tdoa_case_t g_tdoa_cases[] = {
    { "zero sensors", "ESP_ERR_INVALID_ARG_COUNT_LT_4", ESP_ERR_INVALID_ARG, setup_count_zero, false },
    { "one sensor", "ESP_ERR_INVALID_ARG_COUNT_LT_4", ESP_ERR_INVALID_ARG, setup_count_one, false },
    { "two sensors", "ESP_ERR_INVALID_ARG_COUNT_LT_4", ESP_ERR_INVALID_ARG, setup_count_two, false },
    { "three sensors", "ESP_ERR_INVALID_ARG_COUNT_LT_4", ESP_ERR_INVALID_ARG, setup_count_three, false },
    { "four sensors inclusive minimum", "ACCEPT_4_NONCOPLANAR", ESP_OK, setup_count_four, false },
    { "five sensors", "ACCEPT_5_NONCOPLANAR", ESP_OK, NULL, false },
    { "eight sensors inclusive maximum", "ACCEPT_8_NONCOPLANAR", ESP_OK, setup_count_eight, false },
    { "nine sensors", "ESP_ERR_INVALID_ARG_COUNT_GT_8", ESP_ERR_INVALID_ARG, setup_count_nine, false },
    { "null observation array", "ESP_ERR_INVALID_ARG_NULL_OBSERVATIONS", ESP_ERR_INVALID_ARG, setup_null_observations, false },
    { "null config", "ESP_ERR_INVALID_ARG_NULL_CONFIG", ESP_ERR_INVALID_ARG, setup_null_config, false },
    { "null output", "ESP_ERR_INVALID_ARG_NULL_RESULT", ESP_ERR_INVALID_ARG, setup_null_result, false },
    { "zero propagation speed", "ESP_ERR_INVALID_ARG_SPEED_LE_0", ESP_ERR_INVALID_ARG, setup_zero_speed, false },
    { "negative propagation speed", "ESP_ERR_INVALID_ARG_SPEED_LE_0", ESP_ERR_INVALID_ARG, setup_negative_speed, false },
    { "NaN propagation speed", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", ESP_ERR_INVALID_ARG, setup_nan_speed, false },
    { "+Inf propagation speed", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", ESP_ERR_INVALID_ARG, setup_positive_infinite_speed, false },
    { "-Inf propagation speed", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", ESP_ERR_INVALID_ARG, setup_negative_infinite_speed, false },
    { "NaN sensor position", "ESP_ERR_INVALID_ARG_POSITION_NONFINITE", ESP_ERR_INVALID_ARG, setup_nan_x, false },
    { "+Inf sensor position", "ESP_ERR_INVALID_ARG_POSITION_NONFINITE", ESP_ERR_INVALID_ARG, setup_positive_infinite_x, false },
    { "-Inf sensor position", "ESP_ERR_INVALID_ARG_POSITION_NONFINITE", ESP_ERR_INVALID_ARG, setup_negative_infinite_x, false },
    { "NaN initial guess", "ESP_ERR_INVALID_ARG_GUESS_NONFINITE", ESP_ERR_INVALID_ARG, setup_nan_guess, false },
    { "zero iteration limit", "ESP_ERR_INVALID_ARG_ITERATIONS_ZERO", ESP_ERR_INVALID_ARG, setup_zero_iterations, false },
    { "zero convergence distance", "ESP_ERR_INVALID_ARG_CONVERGENCE_LE_0", ESP_ERR_INVALID_ARG, setup_zero_convergence, false },
    { "negative damping", "ESP_ERR_INVALID_ARG_DAMPING_LT_0", ESP_ERR_INVALID_ARG, setup_negative_damping, false },
    { "all sensors colocated", "duplicate_sensor_position", ESP_ERR_INVALID_STATE, setup_all_colocated, false },
    { "collinear sensors", "ESP_ERR_INVALID_STATE_GEOMETRY_NOT_3D", ESP_ERR_INVALID_STATE, setup_collinear, false },
    { "coplanar sensors with colocated points", "duplicate_sensor_position", ESP_ERR_INVALID_STATE, setup_coplanar, false },
    { "near-degenerate depth just below 0.05", "ESP_ERR_INVALID_STATE_DEPTH_THRESHOLD_NOT_MET", ESP_ERR_INVALID_STATE, setup_depth_just_below_threshold, false },
    { "depth ratio exactly 0.05 boundary", "ESP_ERR_INVALID_STATE_DEPTH_THRESHOLD_IS_STRICTLY_GREATER", ESP_ERR_INVALID_STATE, setup_depth_threshold_equal, false },
    { "duplicate positions with remaining 3D geometry", "duplicate_sensor_position", ESP_ERR_INVALID_STATE, setup_duplicate_positions, false },
    { "invalid speed clears old result", "ESP_ERR_INVALID_ARG_SPEED_LE_0", ESP_ERR_INVALID_ARG, setup_stale_result, false },
    { "INT64_MAX epoch loses microsecond precision", "KNOWN_DEFECT_INT64_EPOCH_CAST_TO_DOUBLE", ESP_OK, setup_high_epoch, true },
};

static void test_tdoa_case_matrix(void)
{
    for (size_t i = 0U; i < ARRAY_COUNT(g_tdoa_cases); ++i) {
        const tdoa_case_t *test = &g_tdoa_cases[i];
        tdoa_case_state_t state;
        memset(&state, 0, sizeof(state));
        setup_base(&state);
        if (test->setup != NULL) {
            test->setup(&state);
        }

        const esp_err_t actual = localization_tdoa_solve(
            state.observations_arg,
            state.count,
            state.config_arg,
            state.guess_arg,
            state.result_arg
        );
        if (actual != test->expected_status) {
            printf("FAIL %s expected status=%d actual=%d reason=%s\n",
                test->name, test->expected_status, actual, test->expected_reason);
            fflush(stdout);
        }
        assert(actual == test->expected_status);

        if (strcmp(test->expected_reason, "duplicate_sensor_position") == 0) {
            localization_validation_reason_t reason = LOCALIZATION_VALIDATION_REASON_NONE;
            assert(localization_validate_ex(
                state.observations_arg,
                state.count,
                state.config_arg,
                state.guess_arg,
                &reason
            ) == ESP_ERR_INVALID_STATE);
            assert(reason == LOCALIZATION_INVALID_DUPLICATE_SENSOR_POSITION);
            assert(strcmp(localization_validation_reason_string(reason),
                "duplicate_sensor_position") == 0);
        }

        if (!test->known_defect && actual == ESP_OK) {
            assert(state.result.converged);
            assert(state.result.iterations > 0U);
            assert(state.result.iterations <= state.config.max_iterations);
            assert(point_is_finite(state.result.source_position));
            assert(isfinite(state.result.rms_residual_us));
        } else if (!test->known_defect && state.result_arg != NULL) {
            assert(!state.result.converged);
            assert(state.result.iterations == 0U);
            if (test->setup == setup_stale_result) {
                const unsigned char zero_result[sizeof(state.result)] = { 0U };
                assert(memcmp(&state.result, zero_result, sizeof(state.result)) == 0);
                printf("PASS rejected invalid-speed call clears the full result struct\n");
            }
        }
        if (test->known_defect) {
            assert(strcmp(test->expected_reason,
                "KNOWN_DEFECT_INT64_EPOCH_CAST_TO_DOUBLE") == 0);
            assert(actual == ESP_OK);
            assert(point_is_finite(state.result.source_position));
            /* At this epoch, converting absolute int64 us to double has
             * a 1024 us ULP; valid differential arrivals lose precision. */
            const double source_error_m = distance_m(
                state.result.source_position, state.truth
            );
            assert(source_error_m > 0.02);
            printf("KNOWN_DEFECT_REPRO input=8 sensors, speed=1850 m/s, "
                "emission_us=INT64_MAX-1000000, returned source error=%.6f m, "
                "reported emission_us=%lld\n",
                source_error_m, (long long)state.result.emission_time_us);
            printf("KNOWN_DEFECT %s reason=%s\n", test->name, test->expected_reason);
        } else {
            printf("%s %s reason=%s status=%d\n",
                actual == ESP_OK ? "PASS" : "REJECT",
                test->name, test->expected_reason, actual);
        }
    }
}


static void test_duplicate_position_reason_is_order_independent(void)
{
    tdoa_case_state_t state;
    memset(&state, 0, sizeof(state));
    setup_base(&state);
    setup_duplicate_positions(&state);

    localization_validation_reason_t original_reason = LOCALIZATION_VALIDATION_REASON_NONE;
    assert(localization_validate_ex(state.observations, state.count, &state.config,
        NULL, &original_reason) == ESP_ERR_INVALID_STATE);
    assert(original_reason == LOCALIZATION_INVALID_DUPLICATE_SENSOR_POSITION);

    const localization_observation_t first = state.observations[0];
    state.observations[0] = state.observations[1];
    state.observations[1] = first;
    localization_validation_reason_t permuted_reason = LOCALIZATION_VALIDATION_REASON_NONE;
    assert(localization_validate_ex(state.observations, state.count, &state.config,
        NULL, &permuted_reason) == ESP_ERR_INVALID_STATE);
    assert(permuted_reason == original_reason);
    assert(strcmp(localization_validation_reason_string(permuted_reason),
        "duplicate_sensor_position") == 0);
    puts("PASS duplicate sensor rejection reason is independent of sensor order");
}

static void test_iteration_limit_is_reported(void)
{
    localization_observation_t observations[5];
    const localization_point3d_t source = { 0.25, 0.40, 0.30 };
    make_observations(g_validation_sensors, 5U, source, 340.0, 12000000,
        observations);
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 340.0;
    config.max_iterations = 1U;
    config.convergence_m = 1.0e-12;
    const localization_point3d_t guess = { 10.0, 10.0, 10.0 };
    localization_result_t result;

    assert(localization_tdoa_solve(
        observations, 5U, &config, &guess, &result
    ) == ESP_OK);
    assert(!result.converged);
    assert(result.iterations == 1U);
    assert(point_is_finite(result.source_position));
    assert(isfinite(result.rms_residual_us));
    puts("PASS iteration limit returns an unconverged finite result");
}

static void test_rigid_coordinate_frame_transform(void)
{
    localization_point3d_t transformed_sensors[ARRAY_COUNT(g_sensors_eight)];
    for (size_t i = 0U; i < ARRAY_COUNT(g_sensors_eight); ++i) {
        const localization_point3d_t original = g_sensors_eight[i];
        /* Rz(+90 degrees) followed by a local-frame translation. */
        transformed_sensors[i] = (localization_point3d_t) {
            -original.y_m + 11.0,
            original.x_m - 7.0,
            original.z_m + 3.0,
        };
    }
    const localization_point3d_t original_source = { -1.2, 0.85, -1.45 };
    const localization_point3d_t transformed_source = {
        -original_source.y_m + 11.0,
        original_source.x_m - 7.0,
        original_source.z_m + 3.0,
    };
    const oracle_fixture_t transformed_fixture = {
        "rigidly transformed local frame",
        transformed_sensors,
        ARRAY_COUNT(transformed_sensors),
        transformed_source,
        1200.0,
        3456000,
    };
    run_oracle_fixture(&transformed_fixture);
}

static void test_depth_ratio_equality_boundary(void)
{
    const double scale = sqrt(2.0);
    const double threshold_height = 0.05 * scale * scale * scale;
    const localization_point3d_t equal[4] = {
        { 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 }, { 0.0, 0.0, threshold_height },
    };
    localization_point3d_t above[4];
    memcpy(above, equal, sizeof(above));
    above[3].z_m = nextafter(threshold_height, INFINITY);
    /* The documented depth predicate is strictly greater than 0.05; exact
     * equality fails and the next representable value above it passes. */
    assert(!localization_positions_resolve_depth(equal, ARRAY_COUNT(equal)));
    assert(localization_positions_resolve_depth(above, ARRAY_COUNT(above)));
    puts("PASS depth threshold equality is rejected; next representable value is accepted");
}

int main(void)
{
    static const oracle_fixture_t fixtures[] = {
        {
            "2D source embedded in 3D array",
            g_sensors_2d_source, ARRAY_COUNT(g_sensors_2d_source),
            { 1.25, -0.85, 0.0 }, 343.0, 4200000,
        },
        {
            "3D source at origin",
            g_sensors_origin, ARRAY_COUNT(g_sensors_origin),
            { 0.0, 0.0, 0.0 }, 1850.0, 9000000,
        },
        {
            "signed non-axis-aligned 3D source",
            g_sensors_eight, ARRAY_COUNT(g_sensors_eight),
            { -1.2, 0.85, -1.45 }, 5250.0, 200000,
        },
    };

    for (size_t i = 0U; i < ARRAY_COUNT(fixtures); ++i) {
        run_oracle_fixture(&fixtures[i]);
    }
    test_rigid_coordinate_frame_transform();
    test_tdoa_case_matrix();
    test_duplicate_position_reason_is_order_independent();
    test_depth_ratio_equality_boundary();
    test_iteration_limit_is_reported();
    puts("MAX-132 TDOA oracle and validation tests passed");
    return 0;
}
