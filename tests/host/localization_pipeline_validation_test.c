/*
 * MAX-141: synthetic, deterministic ground-truth validation of the full
 * geophone_events (MAX-134) -> localization_event_bridge (MAX-135/170)
 * pipeline. This is host/synthetic evidence only -- it proves the pipeline
 * behaves correctly and quantifies its own numerical limits on synthetic
 * data; it is explicitly NOT physical HIL/field evidence (no real geophone,
 * no real rubble propagation, no real AD7606 noise floor). Keeping this
 * distinction is the ticket's own explicit requirement.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geophone_events.h"
#include "localization_event_bridge.h"


/*
 * Deterministic xorshift32 PRNG for synthetic background noise. A pure sine
 * is a poor stand-in for noise here: at the project's real 500 Hz rate the
 * STA window (10 samples) is only half of a 20-sample sine period, so a
 * short window's energy depends heavily on which part of the cycle it
 * lands on -- a spurious periodic beat against the STA/LTA ratio that has
 * nothing to do with a real event. Broadband pseudo-random noise (still
 * fully deterministic/reproducible, per the ticket's "deterministic
 * synthetic" requirement) does not have this pathology at any window size.
 */
static uint32_t s_noise_state;

static float next_noise_sample(
    float amplitude
)
{
    s_noise_state ^= s_noise_state << 13;
    s_noise_state ^= s_noise_state >> 17;
    s_noise_state ^= s_noise_state << 5;
    const float unit = (float)(s_noise_state % 20001u) / 10000.0f - 1.0f; /* [-1, 1] */
    return amplitude * unit;
}


static const localization_point3d_t s_sensors4[4] = {
    { 0.0, 0.0, 0.0 },
    { 2.0, 0.0, 0.0 },
    { 0.0, 2.0, 0.0 },
    { 0.0, 0.0, 2.0 }
};
static const localization_point3d_t s_source = { 0.25, 0.40, 0.30 };
static const double s_true_speed_mps = 340.0;


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

    /* Fixed per-channel seed: deterministic across runs, but distinct
     * between channels so their noise is not identically correlated. */
    s_noise_state = 0x9E3779B9u ^ (channel_seed * 0x85EBCA6Bu + 1u);
    for (size_t i = 0u; i < buffer_len; ++i) {
        samples[i] = next_noise_sample(noise_amplitude);
    }

    const size_t end =
        arrival_index + pulse_width > buffer_len
        ? buffer_len
        : arrival_index + pulse_width;
    for (size_t i = arrival_index; i < end; ++i) {
        samples[i] = pulse_amplitude;
    }

    return samples;
}


static double true_arrival_seconds(
    localization_point3d_t sensor,
    double emission_s
)
{
    const double dx = s_source.x_m - sensor.x_m;
    const double dy = s_source.y_m - sensor.y_m;
    const double dz = s_source.z_m - sensor.z_m;
    return emission_s + sqrt(dx * dx + dy * dy + dz * dz) / s_true_speed_mps;
}


typedef struct
{
    localization_event_result_t result;
    long pick_error_samples[4];
    bool pick_found[4];
} scenario_outcome_t;


/*
 * Runs the full pipeline for up to 4 sensors and reports outcome +
 * per-channel pick error against the exact synthetic ground truth.
 *
 * Window lengths are fixed SAMPLE counts, not time, and deliberately do not
 * scale with sample_rate_hz. This test's whole point is to compare arrival
 * *time* precision across sample rates with everything else held equal;
 * scaling the detector's own windows by sample rate would confound that --
 * at 1 MHz a "realistic" 200 ms LTA window is 200,000 samples, large enough
 * to swallow the entire synthetic pulse before the detector has even
 * accumulated enough history to start evaluating it.
 */
static scenario_outcome_t run_scenario(
    size_t sensor_count,
    uint32_t sample_rate_hz,
    double emission_s,
    float pulse_amplitude,
    float noise_amplitude,
    double propagation_speed_for_solve
)
{
    assert(sensor_count <= 4u);

    const size_t sta_w = 10u;
    const size_t lta_w = 100u;
    const size_t noise_w = 50u;
    const size_t pulse_w = 20u;
    const size_t refine_span = sta_w;

    size_t true_index[4] = { 0 };
    size_t buffer_len = 0u;
    for (size_t i = 0u; i < sensor_count; ++i) {
        const double t = true_arrival_seconds(s_sensors4[i], emission_s);
        true_index[i] = (size_t)llround(t * (double)sample_rate_hz);
        const size_t needed = true_index[i] + pulse_w + noise_w + 10u;
        if (needed > buffer_len) {
            buffer_len = needed;
        }
    }

    localization_event_channel_input_t channels[4];
    float *buffers[4] = { NULL, NULL, NULL, NULL };

    const geophone_channel_pick_config_t pick_config = {
        .noise_window_samples = noise_w,
        .sta_window_samples = sta_w,
        .lta_window_samples = lta_w,
        .sta_lta_trigger_ratio = 3.0f,
        .refine_lookback_samples = refine_span,
        .refine_lookahead_samples = refine_span,
        .refine_amplitude_fraction = 0.5f,
        .snr_signal_window_samples = pulse_w,
        .minimum_snr = 3.0f
    };

    scenario_outcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));

    for (size_t i = 0u; i < sensor_count; ++i) {
        buffers[i] = build_channel(
            buffer_len, true_index[i], pulse_w, pulse_amplitude, noise_amplitude, (uint32_t)i
        );

        geophone_channel_pick_t pick;
        const bool accepted = geophone_event_pick_channel_arrival(
            buffers[i], buffer_len, sample_rate_hz, &pick_config, &pick
        );

        outcome.pick_found[i] = accepted;
        if (accepted) {
            outcome.pick_error_samples[i] =
                (long)pick.arrival_sample_index - (long)true_index[i];
        }

        channels[i] = (localization_event_channel_input_t) {
            .pick = pick,
            .sample_rate_hz = sample_rate_hz,
            .sensor_position = s_sensors4[i]
        };
    }

    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = propagation_speed_for_solve;
    config.max_iterations = 100;
    config.convergence_m = 1.0e-7;

    localization_event_solve(
        channels, sensor_count, &config, NULL, &outcome.result
    );

    for (size_t i = 0u; i < sensor_count; ++i) {
        free(buffers[i]);
    }

    return outcome;
}


static double position_error_m(
    const localization_point3d_t *solved
)
{
    const double dx = solved->x_m - s_source.x_m;
    const double dy = solved->y_m - s_source.y_m;
    const double dz = solved->z_m - s_source.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static void test_clean_high_resolution_4_sensor_converges_accurately(void)
{
    /* 1 MHz stands in for a hypothetical hardware-timed/sub-sample-refined
     * acquisition (MAX-142's open question), specifically to prove the
     * pick+solve algorithm itself is correct when time resolution is not
     * the bottleneck. */
    const scenario_outcome_t outcome = run_scenario(
        4u, 1000000u, 0.1, 1.0f, 0.01f, s_true_speed_mps
    );

    for (size_t i = 0u; i < 4u; ++i) {
        assert(outcome.pick_found[i]);
        assert(labs(outcome.pick_error_samples[i]) <= 1);
    }

    assert(outcome.result.status == LOCALIZATION_EVENT_SOLVED);
    assert(outcome.result.solver_result.converged);
    const double error_m = position_error_m(&outcome.result.solver_result.source_position);
    printf(
        "[MAX-141] clean high-res 4-sensor: XYZ error=%.6f m residual=%.3f us iterations=%u\n",
        error_m,
        outcome.result.solver_result.rms_residual_us,
        (unsigned)outcome.result.solver_result.iterations
    );
    assert(error_m < 0.01);
}


static void test_realistic_500hz_quantization_bounds_error(void)
{
    /*
     * At the project's real ~500 Hz acquisition rate, whole-sample arrival
     * picking quantizes time to 2 ms. Over these sensor spacings (a few
     * meters) and this propagation speed, that is frequently *larger* than
     * the true inter-sensor time-of-flight difference -- two of these four
     * sensors' true arrivals round to the identical sample. This is
     * measured evidence for MAX-142's timing-budget question, not merely
     * asserted: this test reports the actual resulting position error
     * rather than assuming a number, and only bounds it loosely (must not
     * silently diverge/NaN) since the whole point is that whole-sample
     * picking here is a real accuracy bottleneck, not a solved problem.
     */
    const scenario_outcome_t outcome = run_scenario(
        4u, 500u, 0.5, 1.0f, 0.01f, s_true_speed_mps
    );

    for (size_t i = 0u; i < 4u; ++i) {
        assert(outcome.pick_found[i]);
    }

    printf(
        "[MAX-141] realistic 500Hz 4-sensor: pick_errors_samples=[%ld,%ld,%ld,%ld] status=%d\n",
        outcome.pick_error_samples[0],
        outcome.pick_error_samples[1],
        outcome.pick_error_samples[2],
        outcome.pick_error_samples[3],
        (int)outcome.result.status
    );

    if (outcome.result.status == LOCALIZATION_EVENT_SOLVED) {
        const double error_m =
            position_error_m(&outcome.result.solver_result.source_position);
        printf(
            "[MAX-141] realistic 500Hz 4-sensor: XYZ error=%.4f m (vs <0.01 m at high-res) "
            "converged=%s\n",
            error_m,
            outcome.result.solver_result.converged ? "true" : "false"
        );
        assert(isfinite(error_m));
        /* The 2 ms/sample quantization at this geometry/speed corresponds
         * to roughly 0.68 m of one-way distance error (2ms * 340 m/s);
         * demanding sub-centimeter accuracy here would be asserting
         * something the current whole-sample pipeline cannot deliver. This
         * loose upper bound exists only to catch a gross regression (e.g.
         * a unit-conversion bug), not to claim this resolution is adequate. */
        assert(error_m < 5.0);
    } else {
        /* A degenerate/duplicate-arrival-time solve is also an acceptable,
         * informative outcome at this resolution -- it must simply not be
         * silently reported as SOLVED with a fabricated position. */
        assert(outcome.result.status == LOCALIZATION_EVENT_SOLVER_REJECTED);
    }
}


static void test_exactly_3_sensors_reports_planar_2d_unavailable_for_3d_method(void)
{
    const scenario_outcome_t outcome = run_scenario(
        3u, 1000000u, 0.1, 1.0f, 0.01f, s_true_speed_mps
    );

    for (size_t i = 0u; i < 3u; ++i) {
        assert(outcome.pick_found[i]);
    }
    assert(outcome.result.status == LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY);
    assert(outcome.result.dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
}


static void test_missing_channel_is_rejected_not_a_false_position(void)
{
    /* Sensor 2's "waveform" is pure noise -- no event ever occurred on it
     * (a disconnected/faulty channel). geophone_events must reject its
     * pick (no false trigger on noise alone), leaving only 3 usable
     * channels for a session physically wired for 4. */
    const size_t sta_w = 10u;
    const size_t lta_w = 100u;
    const size_t noise_w = 50u;
    const size_t buffer_len = 106000u;

    const geophone_channel_pick_config_t pick_config = {
        .noise_window_samples = noise_w,
        .sta_window_samples = sta_w,
        .lta_window_samples = lta_w,
        .sta_lta_trigger_ratio = 3.0f,
        .refine_lookback_samples = sta_w,
        .refine_lookahead_samples = sta_w,
        .refine_amplitude_fraction = 0.5f,
        .snr_signal_window_samples = 20u,
        .minimum_snr = 3.0f
    };

    localization_event_channel_input_t channels[4];
    float *buffers[4];
    for (size_t i = 0u; i < 4u; ++i) {
        const double t = true_arrival_seconds(s_sensors4[i], 0.1);
        const size_t arrival = (size_t)llround(t * 1000000.0);
        const bool inject_event = (i != 2u);

        buffers[i] = build_channel(
            buffer_len,
            arrival,
            20u,
            inject_event ? 1.0f : 0.0f,
            0.01f,
            (uint32_t)i
        );

        geophone_channel_pick_t pick;
        (void)geophone_event_pick_channel_arrival(
            buffers[i], buffer_len, 1000000u, &pick_config, &pick
        );
        channels[i] = (localization_event_channel_input_t) {
            .pick = pick,
            .sample_rate_hz = 1000000u,
            .sensor_position = s_sensors4[i]
        };
    }

    assert(channels[2].pick.status != GEOPHONE_PICK_ACCEPTED);

    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = s_true_speed_mps;
    config.max_iterations = 100;
    config.convergence_m = 1.0e-7;

    localization_event_result_t result;
    localization_event_solve(channels, 4u, &config, NULL, &result);

    assert(result.status == LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY);
    assert(result.channels_used_count == 3u);

    for (size_t i = 0u; i < 4u; ++i) {
        free(buffers[i]);
    }
}


static void test_propagation_speed_error_sensitivity_is_monotonic(void)
{
    /* Sensitivity to propagation-speed error (MAX-139's concern): solving
     * with an assumed speed that differs from the true speed used to
     * generate the synthetic arrivals must not silently produce the
     * correct answer -- and a larger mismatch must not produce a smaller
     * error than a mild one. This is the quantified sensitivity check the
     * ticket asks for, not just a single pass/fail. */
    const scenario_outcome_t exact =
        run_scenario(4u, 1000000u, 0.1, 1.0f, 0.01f, s_true_speed_mps);
    const scenario_outcome_t mild_mismatch =
        run_scenario(4u, 1000000u, 0.1, 1.0f, 0.01f, s_true_speed_mps * 1.05);
    const scenario_outcome_t large_mismatch =
        run_scenario(4u, 1000000u, 0.1, 1.0f, 0.01f, s_true_speed_mps * 1.20);

    assert(exact.result.status == LOCALIZATION_EVENT_SOLVED);
    assert(mild_mismatch.result.status == LOCALIZATION_EVENT_SOLVED);
    assert(large_mismatch.result.status == LOCALIZATION_EVENT_SOLVED);

    const double error_exact =
        position_error_m(&exact.result.solver_result.source_position);
    const double error_mild =
        position_error_m(&mild_mismatch.result.solver_result.source_position);
    const double error_large =
        position_error_m(&large_mismatch.result.solver_result.source_position);

    printf(
        "[MAX-141] propagation-speed sensitivity: error(0%%)=%.6f m "
        "error(+5%%)=%.6f m error(+20%%)=%.6f m\n",
        error_exact, error_mild, error_large
    );

    assert(error_exact < error_mild);
    assert(error_mild < error_large);
}


int main(void)
{
    test_clean_high_resolution_4_sensor_converges_accurately();
    test_realistic_500hz_quantization_bounds_error();
    test_exactly_3_sensors_reports_planar_2d_unavailable_for_3d_method();
    test_missing_channel_is_rejected_not_a_false_position();
    test_propagation_speed_error_sensitivity_is_monotonic();
    puts("localization pipeline validation tests passed");
    return 0;
}
