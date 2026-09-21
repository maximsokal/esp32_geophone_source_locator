/*
 * MAX-137: synthetic ground-truth validation of the semblance/coherency
 * source-location backend. Builds multi-channel synthetic waveforms with a
 * KNOWN source position (a shared pulse shape, time-shifted per channel by
 * the true travel time), then checks the coarse-to-fine grid search
 * recovers a position close to that truth with a high semblance score, and
 * that a deliberately wrong region of the same recording scores lower.
 */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "localization_semblance.h"


#define TEST_PI 3.14159265358979323846


static uint32_t s_noise_state;

static float next_noise_sample(
    float amplitude
)
{
    s_noise_state ^= s_noise_state << 13;
    s_noise_state ^= s_noise_state >> 17;
    s_noise_state ^= s_noise_state << 5;
    const float unit = (float)(s_noise_state % 20001u) / 10000.0f - 1.0f;
    return amplitude * unit;
}


/* A smooth raised-cosine hump, not a flat plateau -- a flat pulse would
 * correlate equally well at several nearby shifts, hiding misalignment.
 * This shape has a single, sharply-located peak so a candidate position
 * that predicts the wrong shift measurably decorrelates the stack. */
static float pulse_shape(
    size_t i,
    size_t width,
    float amplitude
)
{
    if (i >= width) {
        return 0.0f;
    }
    const double phase = 2.0 * TEST_PI * (double)i / (double)(width - 1u);
    return amplitude * (float)(0.5 - 0.5 * cos(phase));
}


static float *build_channel(
    size_t buffer_len,
    size_t arrival_index,
    size_t pulse_width,
    float pulse_amplitude,
    float noise_amplitude,
    uint32_t channel_seed
)
{
    float *samples = calloc(buffer_len, sizeof(float));
    assert(samples != NULL);

    s_noise_state = 0x9E3779B9u ^ (channel_seed * 0x85EBCA6Bu + 1u);
    for (size_t i = 0u; i < buffer_len; ++i) {
        samples[i] = next_noise_sample(noise_amplitude);
    }

    const size_t end =
        arrival_index + pulse_width > buffer_len
        ? buffer_len
        : arrival_index + pulse_width;
    for (size_t i = arrival_index; i < end; ++i) {
        samples[i] += pulse_shape(i - arrival_index, pulse_width, pulse_amplitude);
    }

    return samples;
}


typedef struct
{
    localization_semblance_channel_t channels[LOCALIZATION_SEMBLANCE_MAX_CHANNELS];
    float *buffers[LOCALIZATION_SEMBLANCE_MAX_CHANNELS];
    size_t channel_count;
} synthetic_recording_t;


static synthetic_recording_t build_recording(
    const localization_point3d_t *sensors,
    size_t sensor_count,
    localization_point3d_t source,
    double speed_mps,
    double emission_s,
    uint32_t sample_rate_hz,
    size_t pulse_width,
    size_t buffer_len,
    size_t *out_reference_sample_index
)
{
    synthetic_recording_t recording;
    memset(&recording, 0, sizeof(recording));
    recording.channel_count = sensor_count;

    const size_t reference_sample_index =
        (size_t)llround(emission_s * (double)sample_rate_hz);
    *out_reference_sample_index = reference_sample_index;

    for (size_t ch = 0u; ch < sensor_count; ++ch) {
        const double dx = source.x_m - sensors[ch].x_m;
        const double dy = source.y_m - sensors[ch].y_m;
        const double dz = source.z_m - sensors[ch].z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        const size_t shift_samples =
            (size_t)llround((distance / speed_mps) * (double)sample_rate_hz);
        const size_t arrival_index = reference_sample_index + shift_samples;
        assert(arrival_index + pulse_width < buffer_len);

        recording.buffers[ch] = build_channel(
            buffer_len, arrival_index, pulse_width, 1.0f, 0.02f, (uint32_t)ch
        );
        recording.channels[ch] = (localization_semblance_channel_t) {
            .samples = recording.buffers[ch],
            .sample_count = buffer_len,
            .sample_rate_hz = sample_rate_hz,
            .sensor_position = sensors[ch]
        };
    }

    return recording;
}


static void free_recording(
    synthetic_recording_t *recording
)
{
    for (size_t ch = 0u; ch < recording->channel_count; ++ch) {
        free(recording->buffers[ch]);
        recording->buffers[ch] = NULL;
    }
}


static double position_error_m(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static const localization_point3d_t s_sensors_planar[4] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.0 },
    { 0.0, 4.0, 0.0 },
    { 4.0, 4.0, 0.0 }
};
static const localization_point3d_t s_source_planar = { 1.5, 2.0, 0.0 };
static const double s_speed_mps = 340.0;
static const uint32_t s_sample_rate_hz = 5000u;
static const double s_emission_s = 0.05;
static const size_t s_pulse_width = 9u;
static const size_t s_buffer_len = 450u;


static void test_planar_search_recovers_known_source_with_high_semblance(void)
{
    size_t reference_sample_index = 0u;
    synthetic_recording_t recording = build_recording(
        s_sensors_planar, 4u, s_source_planar, s_speed_mps, s_emission_s,
        s_sample_rate_hz, s_pulse_width, s_buffer_len, &reference_sample_index
    );

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    const esp_err_t err = localization_semblance_search(
        recording.channels, recording.channel_count, &bounds, &config, &result
    );

    assert(err == ESP_OK);
    assert(result.found);
    assert(result.best_position.z_m == 0.0);

    const double error_m = position_error_m(result.best_position, s_source_planar);
    printf(
        "[MAX-137] planar search: best=(%.3f,%.3f) true=(%.3f,%.3f) "
        "error=%.4f m semblance=%.4f worst=%.4f candidates=%zu\n",
        result.best_position.x_m, result.best_position.y_m,
        s_source_planar.x_m, s_source_planar.y_m,
        error_m, result.best_semblance, result.worst_semblance,
        result.candidates_searched
    );

    assert(error_m < 0.2);
    assert(result.best_semblance > 0.8);
    assert(result.worst_semblance < result.best_semblance);
    assert(result.candidates_searched > 0u);

    /* A region of the SAME recording far from the true source must score
     * measurably lower -- this backend must discriminate, not just report
     * a number. */
    const localization_semblance_search_bounds_t wrong_region = {
        .min_corner = { 3.8, 3.8, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.1
    };
    localization_semblance_result_t wrong_result;
    assert(
        localization_semblance_search(
            recording.channels, recording.channel_count, &wrong_region, &config,
            &wrong_result
        ) == ESP_OK
    );
    assert(wrong_result.found);
    printf(
        "[MAX-137] wrong region semblance=%.4f (vs true-region best=%.4f)\n",
        wrong_result.best_semblance, result.best_semblance
    );
    assert(wrong_result.best_semblance < result.best_semblance);

    free_recording(&recording);
}


static void test_3d_search_recovers_known_source(void)
{
    static const localization_point3d_t sensors3d[4] = {
        { 0.0, 0.0, 0.0 },
        { 4.0, 0.0, 0.0 },
        { 0.0, 4.0, 0.0 },
        { 2.0, 2.0, 4.0 }
    };
    static const localization_point3d_t source3d = { 1.0, 1.0, 1.0 };

    size_t reference_sample_index = 0u;
    synthetic_recording_t recording = build_recording(
        sensors3d, 4u, source3d, s_speed_mps, s_emission_s, s_sample_rate_hz,
        s_pulse_width, s_buffer_len, &reference_sample_index
    );

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 4.0 },
        .step_m = 1.0
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    assert(
        localization_semblance_search(
            recording.channels, recording.channel_count, &bounds, &config, &result
        ) == ESP_OK
    );
    assert(result.found);

    const double error_m = position_error_m(result.best_position, source3d);
    printf(
        "[MAX-137] 3D search: best=(%.3f,%.3f,%.3f) error=%.4f m semblance=%.4f\n",
        result.best_position.x_m, result.best_position.y_m, result.best_position.z_m,
        error_m, result.best_semblance
    );
    assert(error_m < 0.5);
    assert(result.best_semblance > 0.6);

    free_recording(&recording);
}


static void test_channel_amplitude_differences_do_not_prevent_recovery(void)
{
    /* Real geometric attenuation: amplitude falls off with distance, so the
     * nearest and farthest sensors see very different signal levels. The
     * ticket requires this backend to be tested against amplitude
     * differences, not just equal-amplitude synthetic channels. */
    const size_t reference_sample_index =
        (size_t)llround(s_emission_s * (double)s_sample_rate_hz);

    localization_semblance_channel_t channels[4];
    float *buffers[4];

    for (size_t ch = 0u; ch < 4u; ++ch) {
        const double dx = s_source_planar.x_m - s_sensors_planar[ch].x_m;
        const double dy = s_source_planar.y_m - s_sensors_planar[ch].y_m;
        const double dz = s_source_planar.z_m - s_sensors_planar[ch].z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        const size_t shift_samples =
            (size_t)llround((distance / s_speed_mps) * (double)s_sample_rate_hz);
        const size_t arrival_index = reference_sample_index + shift_samples;
        const float amplitude = (float)(3.0 / (1.0 + distance));

        buffers[ch] = build_channel(
            s_buffer_len, arrival_index, s_pulse_width, amplitude, 0.02f, (uint32_t)ch
        );
        channels[ch] = (localization_semblance_channel_t) {
            .samples = buffers[ch],
            .sample_count = s_buffer_len,
            .sample_rate_hz = s_sample_rate_hz,
            .sensor_position = s_sensors_planar[ch]
        };
    }

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    assert(
        localization_semblance_search(channels, 4u, &bounds, &config, &result) ==
        ESP_OK
    );
    assert(result.found);

    const double error_m = position_error_m(result.best_position, s_source_planar);
    printf(
        "[MAX-137] amplitude-mismatch search: error=%.4f m semblance=%.4f\n",
        error_m, result.best_semblance
    );
    assert(error_m < 0.3);
    assert(result.best_semblance > 0.5);

    for (size_t ch = 0u; ch < 4u; ++ch) {
        free(buffers[ch]);
    }
}


static void test_weaker_echo_does_not_hijack_the_estimate(void)
{
    /* A weaker, noisier secondary arrival from a DIFFERENT position (a
     * reflection/echo) is superimposed on every channel's true direct
     * arrival. The search must still lock onto the stronger, cleaner
     * primary source, not be pulled toward the echo's geometry. */
    static const localization_point3d_t echo_source = { 3.2, 3.4, 0.0 };
    const float primary_amplitude = 1.0f;
    const float echo_amplitude = 0.35f;
    const float background_noise_amplitude = 0.15f;

    const size_t reference_sample_index =
        (size_t)llround(s_emission_s * (double)s_sample_rate_hz);

    localization_semblance_channel_t channels[4];
    float *buffers[4];

    for (size_t ch = 0u; ch < 4u; ++ch) {
        double dx = s_source_planar.x_m - s_sensors_planar[ch].x_m;
        double dy = s_source_planar.y_m - s_sensors_planar[ch].y_m;
        double dz = s_source_planar.z_m - s_sensors_planar[ch].z_m;
        double distance = sqrt(dx * dx + dy * dy + dz * dz);
        size_t shift_samples =
            (size_t)llround((distance / s_speed_mps) * (double)s_sample_rate_hz);
        const size_t primary_index = reference_sample_index + shift_samples;

        buffers[ch] = build_channel(
            s_buffer_len, primary_index, s_pulse_width, primary_amplitude,
            background_noise_amplitude, (uint32_t)ch
        );

        dx = echo_source.x_m - s_sensors_planar[ch].x_m;
        dy = echo_source.y_m - s_sensors_planar[ch].y_m;
        dz = echo_source.z_m - s_sensors_planar[ch].z_m;
        distance = sqrt(dx * dx + dy * dy + dz * dz);
        shift_samples =
            (size_t)llround((distance / s_speed_mps) * (double)s_sample_rate_hz);
        const size_t echo_index = reference_sample_index + shift_samples;
        const size_t echo_end =
            echo_index + s_pulse_width > s_buffer_len
            ? s_buffer_len
            : echo_index + s_pulse_width;
        for (size_t i = echo_index; i < echo_end; ++i) {
            buffers[ch][i] += pulse_shape(i - echo_index, s_pulse_width, echo_amplitude);
        }

        channels[ch] = (localization_semblance_channel_t) {
            .samples = buffers[ch],
            .sample_count = s_buffer_len,
            .sample_rate_hz = s_sample_rate_hz,
            .sensor_position = s_sensors_planar[ch]
        };
    }

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    assert(
        localization_semblance_search(channels, 4u, &bounds, &config, &result) ==
        ESP_OK
    );
    assert(result.found);

    const double primary_error_m = position_error_m(result.best_position, s_source_planar);
    const double echo_error_m = position_error_m(result.best_position, echo_source);
    printf(
        "[MAX-137] echo-robustness search: best=(%.3f,%.3f) primary_error=%.4f m "
        "echo_error=%.4f m semblance=%.4f\n",
        result.best_position.x_m, result.best_position.y_m,
        primary_error_m, echo_error_m, result.best_semblance
    );
    assert(primary_error_m < echo_error_m);
    assert(primary_error_m < 0.3);

    for (size_t ch = 0u; ch < 4u; ++ch) {
        free(buffers[ch]);
    }
}


static void test_rejects_too_few_channels(void)
{
    size_t reference_sample_index = 0u;
    synthetic_recording_t recording = build_recording(
        s_sensors_planar, 1u, s_source_planar, s_speed_mps, s_emission_s,
        s_sample_rate_hz, s_pulse_width, s_buffer_len, &reference_sample_index
    );

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    assert(
        localization_semblance_search(
            recording.channels, 1u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    free_recording(&recording);
}


static void test_rejects_mismatched_sample_rate(void)
{
    size_t reference_sample_index = 0u;
    synthetic_recording_t recording = build_recording(
        s_sensors_planar, 4u, s_source_planar, s_speed_mps, s_emission_s,
        s_sample_rate_hz, s_pulse_width, s_buffer_len, &reference_sample_index
    );
    recording.channels[1].sample_rate_hz = s_sample_rate_hz * 2u;

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };

    localization_semblance_result_t result;
    assert(
        localization_semblance_search(
            recording.channels, recording.channel_count, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    free_recording(&recording);
}


static void test_rejects_invalid_config_and_null_arguments(void)
{
    size_t reference_sample_index = 0u;
    synthetic_recording_t recording = build_recording(
        s_sensors_planar, 4u, s_source_planar, s_speed_mps, s_emission_s,
        s_sample_rate_hz, s_pulse_width, s_buffer_len, &reference_sample_index
    );

    const localization_semblance_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    localization_semblance_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .reference_sample_index = reference_sample_index,
        .window_half_span_samples = 15u,
        .refine_divisor = 5u
    };
    localization_semblance_result_t result;

    assert(
        localization_semblance_search(NULL, 4u, &bounds, &config, &result) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_semblance_search(
            recording.channels, 4u, NULL, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, NULL, &result
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &config, NULL
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_config_t bad_speed = config;
    bad_speed.propagation_speed_mps = 0.0;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &bad_speed, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_config_t bad_divisor = config;
    bad_divisor.refine_divisor = 1u;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &bad_divisor, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_search_bounds_t bad_step = bounds;
    bad_step.step_m = 0.0;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bad_step, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_config_t bad_nonfinite_speed = config;
    bad_nonfinite_speed.propagation_speed_mps = NAN;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &bad_nonfinite_speed, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_config_t bad_window = config;
    bad_window.window_half_span_samples = SIZE_MAX;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &bad_window, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_search_bounds_t reversed_bounds = bounds;
    reversed_bounds.max_corner.x_m = reversed_bounds.min_corner.x_m - 1.0;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &reversed_bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_semblance_search_bounds_t huge_bounds = bounds;
    huge_bounds.max_corner.x_m = DBL_MAX;
    huge_bounds.step_m = 1.0;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &huge_bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    const float saved_sample = recording.buffers[0][0];
    recording.buffers[0][0] = NAN;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
    recording.buffers[0][0] = saved_sample;

    const localization_point3d_t saved_position = recording.channels[0].sensor_position;
    recording.channels[0].sensor_position.x_m = INFINITY;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
    recording.channels[0].sensor_position = saved_position;

    const uint32_t saved_rate = recording.channels[0].sample_rate_hz;
    recording.channels[0].sample_rate_hz = 0u;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
    recording.channels[0].sample_rate_hz = saved_rate;

    const float *saved_samples = recording.channels[0].samples;
    recording.channels[0].samples = NULL;
    assert(
        localization_semblance_search(
            recording.channels, 4u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
    recording.channels[0].samples = saved_samples;

    free_recording(&recording);
}


int main(void)
{
    test_planar_search_recovers_known_source_with_high_semblance();
    test_3d_search_recovers_known_source();
    test_channel_amplitude_differences_do_not_prevent_recovery();
    test_weaker_echo_does_not_hijack_the_estimate();
    test_rejects_too_few_channels();
    test_rejects_mismatched_sample_rate();
    test_rejects_invalid_config_and_null_arguments();
    puts("localization semblance tests passed");
    return 0;
}
