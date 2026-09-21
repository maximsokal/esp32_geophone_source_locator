#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "localization_event_bridge.h"


static geophone_channel_pick_t accepted_pick(
    size_t sample_index
)
{
    return (geophone_channel_pick_t) {
        .status = GEOPHONE_PICK_ACCEPTED,
        .method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED,
        .arrival_sample_index = sample_index,
        .arrival_time_s = 0.0f,
        .snr = 20.0f,
        .uncertainty_samples = 10u
    };
}


static geophone_channel_pick_t rejected_pick(void)
{
    return (geophone_channel_pick_t) {
        .status = GEOPHONE_PICK_REJECTED_LOW_SNR,
        .method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED,
        .arrival_sample_index = 0u,
        .arrival_time_s = 0.0f,
        .snr = 1.0f,
        .uncertainty_samples = 10u
    };
}


/* Ground truth: source (0.25, 0.40, 0.30) m, 4 sensors at the unit-ish
 * tetrahedron corners used by localization_tdoa_test.c, speed 340 m/s,
 * emission at 0.05 s. Sample indices below are
 * round((emission_s + distance/speed) * 1e6 * sample_rate_hz / 1e6) at a
 * 1 MHz sample_rate_hz -- high resolution is used here specifically to give
 * the *test* enough precision to prove the bridge's conversion and
 * plumbing; the whole-sample-index picks MAX-134 currently produces at a
 * real ~500 Hz acquisition rate would need the cross-correlation
 * sub-sample refinement (already implemented, not yet wired here) for this
 * level of real-world TDOA precision. */
#define TEST_SAMPLE_RATE_HZ 1000000u

static const localization_point3d_t s_sensors[4] = {
    { 0.0, 0.0, 0.0 },
    { 2.0, 0.0, 0.0 },
    { 0.0, 2.0, 0.0 },
    { 0.0, 0.0, 2.0 }
};
static const size_t s_sample_indices[4] = { 51644u, 55353u, 54844u, 55189u };
static const localization_point3d_t s_expected_source = { 0.25, 0.40, 0.30 };


static localization_config_t test_config(void)
{
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 340.0;
    config.max_iterations = 100;
    config.convergence_m = 1.0e-7;
    return config;
}


static void assert_near(double actual, double expected, double epsilon)
{
    assert(fabs(actual - expected) <= epsilon);
}


static void test_solves_when_all_channels_are_accepted(void)
{
    localization_event_channel_input_t channels[4];
    for (size_t i = 0u; i < 4u; ++i) {
        channels[i] = (localization_event_channel_input_t) {
            .pick = accepted_pick(s_sample_indices[i]),
            .sample_rate_hz = TEST_SAMPLE_RATE_HZ,
            .sensor_position = s_sensors[i]
        };
    }

    const localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(channels, 4u, &config, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EVENT_SOLVED);
    assert(result.channel_mask_used == 0x0Fu);
    assert(result.channel_mask_rejected == 0u);
    assert(result.channels_used_count == 4u);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(result.solver_result.converged);
    assert_near(result.solver_result.source_position.x_m, s_expected_source.x_m, 0.01);
    assert_near(result.solver_result.source_position.y_m, s_expected_source.y_m, 0.01);
    assert_near(result.solver_result.source_position.z_m, s_expected_source.z_m, 0.01);
}


static void test_one_rejected_pick_leaves_planar_2d_method_unavailable(void)
{
    /* MAX-170: exactly 3 remaining accepted channels is a valid, supported
     * planar/2D session -- not a generic "insufficient data" failure. The
     * wired TDOA solver specifically requires 3D, so it reports the more
     * precise METHOD_UNAVAILABLE_FOR_DIMENSIONALITY status instead. */
    localization_event_channel_input_t channels[4];
    for (size_t i = 0u; i < 4u; ++i) {
        channels[i] = (localization_event_channel_input_t) {
            .pick = accepted_pick(s_sample_indices[i]),
            .sample_rate_hz = TEST_SAMPLE_RATE_HZ,
            .sensor_position = s_sensors[i]
        };
    }
    channels[1].pick = rejected_pick();

    const localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(channels, 4u, &config, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
    assert(result.channel_mask_used == 0x0Du); /* bits 0,2,3 */
    assert(result.channel_mask_rejected == 0x02u); /* bit 1 */
    assert(result.channels_used_count == 3u);
}


static void test_accepted_pick_with_unknown_sample_rate_is_excluded(void)
{
    localization_event_channel_input_t channels[4];
    for (size_t i = 0u; i < 4u; ++i) {
        channels[i] = (localization_event_channel_input_t) {
            .pick = accepted_pick(s_sample_indices[i]),
            .sample_rate_hz = TEST_SAMPLE_RATE_HZ,
            .sensor_position = s_sensors[i]
        };
    }
    channels[2].sample_rate_hz = 0u; /* accepted pick, but no known rate */

    const localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(channels, 4u, &config, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
    assert(result.channel_mask_rejected == 0x04u); /* bit 2 */
    assert(result.channels_used_count == 3u);
}


static void test_two_channels_is_genuinely_insufficient(void)
{
    localization_event_channel_input_t channels[4];
    for (size_t i = 0u; i < 4u; ++i) {
        channels[i] = (localization_event_channel_input_t) {
            .pick = accepted_pick(s_sample_indices[i]),
            .sample_rate_hz = TEST_SAMPLE_RATE_HZ,
            .sensor_position = s_sensors[i]
        };
    }
    channels[1].pick = rejected_pick();
    channels[2].pick = rejected_pick();

    const localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(channels, 4u, &config, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
    assert(result.channels_used_count == 2u);
}


static void test_degenerate_coplanar_geometry_is_solver_rejected(void)
{
    localization_event_channel_input_t channels[4] = {
        { accepted_pick(1000u), TEST_SAMPLE_RATE_HZ, { 0.0, 0.0, 0.0 } },
        { accepted_pick(1100u), TEST_SAMPLE_RATE_HZ, { 1.0, 0.0, 0.0 } },
        { accepted_pick(1050u), TEST_SAMPLE_RATE_HZ, { 0.0, 1.0, 0.0 } },
        { accepted_pick(1075u), TEST_SAMPLE_RATE_HZ, { 1.0, 1.0, 0.0 } } /* z=0: coplanar */
    };

    localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(channels, 4u, &config, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EVENT_SOLVER_REJECTED);
    assert(result.channels_used_count == 4u);
}


static void test_rejects_invalid_arguments(void)
{
    localization_event_channel_input_t channels[4] = { { 0 } };
    const localization_config_t config = test_config();
    localization_event_result_t result;

    assert(localization_event_solve(NULL, 4u, &config, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_event_solve(channels, 4u, NULL, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_event_solve(channels, 4u, &config, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_event_solve(channels, 0u, &config, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_event_solve(channels, 33u, &config, NULL, &result) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_solves_when_all_channels_are_accepted();
    test_one_rejected_pick_leaves_planar_2d_method_unavailable();
    test_accepted_pick_with_unknown_sample_rate_is_excluded();
    test_two_channels_is_genuinely_insufficient();
    test_degenerate_coplanar_geometry_is_solver_rejected();
    test_rejects_invalid_arguments();
    puts("localization event bridge tests passed");
    return 0;
}
