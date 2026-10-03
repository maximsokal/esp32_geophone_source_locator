#include "geophone_events.h"
#include "seismic_dsp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum
{
    SAMPLE_RATE_HZ = 1000u,
    SAMPLE_COUNT = 2000u,
    EVENT_INDEX = 600u,
    EVENT_TOLERANCE_SAMPLES = 12u
};

#define PI_F 3.14159265358979323846f
#define CHECK(case_name, seed, condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s seed=0x%08x: %s (%s:%d)\n", \
                    (case_name), (unsigned)(seed), #condition, __FILE__, __LINE__); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)


static geophone_channel_pick_config_t pick_config(void)
{
    return (geophone_channel_pick_config_t) {
        .noise_window_samples = 400u,
        .sta_window_samples = 12u,
        .lta_window_samples = 120u,
        .sta_lta_trigger_ratio = 3.0f,
        .refine_lookback_samples = 20u,
        .refine_lookahead_samples = 40u,
        .refine_amplitude_fraction = 0.5f,
        .snr_signal_window_samples = 32u,
        .minimum_snr = 5.0f
    };
}


static uint32_t rng_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}


static float rng_signed(uint32_t *state)
{
    const float unit = (float)(rng_next(state) >> 8) / 16777216.0f;
    return 2.0f * unit - 1.0f;
}


static void fill_white_noise(float *samples, size_t count, uint32_t seed, float amplitude)
{
    uint32_t state = seed;
    for (size_t i = 0u; i < count; ++i) {
        samples[i] = amplitude * rng_signed(&state);
    }
}


/* Eight-sample linear rise plus a plateau gives a known first nonzero sample.
 * At Fs=1 kHz and refine fraction 0.5, the threshold crossing is at most four
 * samples after that truth. The 12-sample tolerance is one STA window, which
 * bounds the coarse trigger's onset displacement while the 20-sample refine
 * lookback still contains the onset. */
static void add_ramp_pulse(
    float *samples,
    size_t count,
    size_t onset,
    size_t width,
    float amplitude
)
{
    const size_t rise = 8u;
    for (size_t k = 0u; k < width && onset + k < count; ++k) {
        const float envelope = k < rise ? (float)(k + 1u) / (float)rise : 1.0f;
        samples[onset + k] += amplitude * envelope;
    }
}


static double sample_position(const geophone_channel_pick_t *pick)
{
    return (double)pick->arrival_sample_index + (double)pick->arrival_subsample_offset;
}


static void check_finite_samples(const char *name, uint32_t seed, const float *samples, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        CHECK(name, seed, isfinite(samples[i]));
    }
}


static void check_finite_pick(const char *name, uint32_t seed, const geophone_channel_pick_t *pick)
{
    CHECK(name, seed, isfinite(pick->arrival_subsample_offset));
    CHECK(name, seed, isfinite(pick->arrival_time_s));
    CHECK(name, seed, isfinite(pick->snr));
}


static const char *status_name(geophone_pick_status_t status)
{
    switch (status) {
    case GEOPHONE_PICK_ACCEPTED: return "ACCEPTED";
    case GEOPHONE_PICK_REJECTED_NO_TRIGGER: return "REJECTED_NO_TRIGGER";
    case GEOPHONE_PICK_REJECTED_NO_REFINEMENT: return "REJECTED_NO_REFINEMENT";
    case GEOPHONE_PICK_REJECTED_LOW_SNR: return "REJECTED_LOW_SNR";
    case GEOPHONE_PICK_REJECTED_INCONSISTENT: return "REJECTED_INCONSISTENT";
    }
    return "UNKNOWN";
}


static double check_event_pick(
    const char *name,
    uint32_t seed,
    const float *samples,
    size_t count,
    const geophone_channel_pick_config_t *config,
    size_t truth,
    size_t tolerance
)
{
    check_finite_samples(name, seed, samples, count);
    geophone_channel_pick_t pick;
    const bool accepted = geophone_event_pick_channel_arrival(
        samples, count, SAMPLE_RATE_HZ, config, &pick
    );
    check_finite_pick(name, seed, &pick);
    CHECK(name, seed, accepted);
    CHECK(name, seed, pick.status == GEOPHONE_PICK_ACCEPTED);
    CHECK(name, seed, pick.arrival_time_s >= 0.0f);

    const double error = sample_position(&pick) - (double)truth;
    CHECK(name, seed, fabs(error) <= (double)tolerance);
    CHECK(name, seed, fabs((double)pick.arrival_time_s - sample_position(&pick) / SAMPLE_RATE_HZ) < 1.0e-5);
    return fabs(error);
}


static geophone_pick_status_t check_rejected_pick(
    const char *name,
    uint32_t seed,
    const float *samples,
    size_t count,
    const geophone_channel_pick_config_t *config
)
{
    check_finite_samples(name, seed, samples, count);
    geophone_channel_pick_t pick;
    const bool accepted = geophone_event_pick_channel_arrival(
        samples, count, SAMPLE_RATE_HZ, config, &pick
    );
    check_finite_pick(name, seed, &pick);
    CHECK(name, seed, !accepted);
    CHECK(name, seed, pick.status != GEOPHONE_PICK_ACCEPTED);
    return pick.status;
}


static void report_error(const char *name, double max_error)
{
    printf("CASE %s detection=ACCEPTED max_pick_error_samples=%.3f\n", name, max_error);
}


static void report_rejection(const char *name, geophone_pick_status_t status)
{
    printf("CASE %s detection=%s pick_error_samples=NA\n", name, status_name(status));
}


static void test_c1_all_zeros(void)
{
    const uint32_t seed = 0x13200101u;
    float samples[SAMPLE_COUNT] = { 0.0f };
    const geophone_channel_pick_config_t config = pick_config();
    const geophone_pick_status_t status = check_rejected_pick("C1", seed, samples, SAMPLE_COUNT, &config);
    CHECK("C1", seed, status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
    report_rejection("C1", status);
}


static void test_c2_constant_dc(void)
{
    const uint32_t seed = 0x13200202u;
    float samples[SAMPLE_COUNT];
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) samples[i] = 0.25f;
    const geophone_channel_pick_config_t config = pick_config();
    const geophone_pick_status_t status = check_rejected_pick("C2", seed, samples, SAMPLE_COUNT, &config);
    CHECK("C2", seed, status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
    report_rejection("C2", status);
}


static void test_c3_white_noise_snr_sweep(void)
{
    static const uint32_t seeds[] = {
        0xC3000101u, 0xC3000102u, 0xC3000103u, 0xC3000104u,
        0xC3000105u, 0xC3000106u, 0xC3000107u, 0xC3000108u
    };
    static const float amplitudes[] = { 0.40f, 0.08f, 0.015f };
    static const char *const levels[] = { "high", "mid", "below-threshold" };
    size_t noise_false_positives = 0u;
    size_t weak_false_positives = 0u;
    size_t strong_false_negatives = 0u;
    double max_error[2] = { 0.0, 0.0 };
    const geophone_channel_pick_config_t config = pick_config();

    for (size_t trial = 0u; trial < sizeof(seeds) / sizeof(seeds[0]); ++trial) {
        float samples[SAMPLE_COUNT];
        fill_white_noise(samples, SAMPLE_COUNT, seeds[trial], 0.02f);
        const geophone_pick_status_t status = check_rejected_pick(
            "C3-white-noise-only", seeds[trial], samples, SAMPLE_COUNT, &config
        );
        if (status == GEOPHONE_PICK_ACCEPTED) ++noise_false_positives;
    }

    for (size_t level = 0u; level < 3u; ++level) {
        size_t accepted_count = 0u;
        for (size_t trial = 0u; trial < sizeof(seeds) / sizeof(seeds[0]); ++trial) {
            float samples[SAMPLE_COUNT];
            const uint32_t seed = seeds[trial] ^ ((uint32_t)level * 0x9E3779B9u);
            fill_white_noise(samples, SAMPLE_COUNT, seed, 0.02f);
            add_ramp_pulse(samples, SAMPLE_COUNT, EVENT_INDEX, 40u, amplitudes[level]);
            check_finite_samples("C3", seed, samples, SAMPLE_COUNT);
            geophone_channel_pick_t pick;
            const bool accepted = geophone_event_pick_channel_arrival(
                samples, SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &pick
            );
            check_finite_pick("C3", seed, &pick);

            if (level < 2u) {
                if (!accepted) ++strong_false_negatives;
                CHECK("C3", seed, accepted);
                CHECK("C3", seed, pick.status == GEOPHONE_PICK_ACCEPTED);
                const double error = fabs(sample_position(&pick) - (double)EVENT_INDEX);
                CHECK("C3", seed, error <= EVENT_TOLERANCE_SAMPLES);
                if (error > max_error[level]) max_error[level] = error;
                ++accepted_count;
            } else {
                if (accepted) ++weak_false_positives;
                CHECK("C3", seed, !accepted);
                CHECK("C3", seed, pick.status == GEOPHONE_PICK_REJECTED_LOW_SNR ||
                                  pick.status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
            }
        }
        if (level < 2u) {
            printf("CASE C3 white_noise_%s detection=%zu/8 max_pick_error_samples=%.3f\n",
                   levels[level], accepted_count, max_error[level]);
        } else {
            printf("CASE C3 white_noise_%s detection=%zu/8 pick_error_samples=NA\n",
                   levels[level], accepted_count);
        }
    }

    CHECK("C3", seeds[0], noise_false_positives == 0u);
    CHECK("C3", seeds[0], weak_false_positives == 0u);
    CHECK("C3", seeds[0], strong_false_negatives == 0u);
    printf("SWEEP C3 noise_only_false_positives=%zu/8 below_threshold_false_positives=%zu/8 "
           "high_mid_false_negatives=%zu/16\n",
           noise_false_positives, weak_false_positives, strong_false_negatives);
}


static void test_c4_colored_drift(void)
{
    const uint32_t seed = 0x13200404u;
    float samples[SAMPLE_COUNT];
    fill_white_noise(samples, SAMPLE_COUNT, seed, 0.006f);
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        const float time_s = (float)i / (float)SAMPLE_RATE_HZ;
        samples[i] += 0.20f * ((float)i / (float)SAMPLE_COUNT - 0.5f);
        samples[i] += 0.025f * sinf(2.0f * PI_F * 0.7f * time_s);
    }
    add_ramp_pulse(samples, SAMPLE_COUNT, EVENT_INDEX, 40u, 0.8f);
    check_finite_samples("C4", seed, samples, SAMPLE_COUNT);
    const geophone_channel_pick_config_t config = pick_config();
    report_error("C4", check_event_pick(
        "C4", seed, samples, SAMPLE_COUNT, &config, EVENT_INDEX, EVENT_TOLERANCE_SAMPLES
    ));
}


static void test_c5_configured_50hz_notch_and_bandpass(void)
{
    /* Synthetic-only mains proof: this test explicitly supplies 50 Hz notch,
     * Q, bandpass corners, and sample rate. It is not deployed acceptance and
     * does not claim these are production defaults or calibrated settings. */
    const uint32_t seed = 0x13200505u;
    float samples[SAMPLE_COUNT];
    uint32_t state = seed;
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        const float time_s = (float)i / (float)SAMPLE_RATE_HZ;
        samples[i] = 0.02f * rng_signed(&state) + 0.30f * sinf(2.0f * PI_F * 50.0f * time_s);
    }
    add_ramp_pulse(samples, SAMPLE_COUNT, EVENT_INDEX, 40u, 0.9f);

    const seismic_dsp_config_t dsp_config = {
        .sample_rate_hz = SAMPLE_RATE_HZ,
        .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL |
                            SEISMIC_DSP_STAGE_BANDPASS |
                            SEISMIC_DSP_STAGE_NOTCH,
        .bandpass = { .low_cutoff_hz = 8.0f, .high_cutoff_hz = 180.0f },
        .notch = { .center_hz = 50.0f, .q_factor = 25.0f }
    };
    CHECK("C5", seed, seismic_dsp_init(&dsp_config) == ESP_OK);
    seismic_dsp_result_t dsp_result;
    CHECK("C5", seed, seismic_dsp_process_block(samples, SAMPLE_COUNT, false, &dsp_result) == ESP_OK);
    CHECK("C5", seed, (dsp_result.completed_stages & dsp_config.requested_stages) == dsp_config.requested_stages);
    check_finite_samples("C5", seed, samples, SAMPLE_COUNT);

    const geophone_channel_pick_config_t config = pick_config();
    /* Two configured 12-sample STA windows (24 ms at 1 kHz) bound filter
     * transient movement before the refiner's local threshold crossing. */
    report_error("C5", check_event_pick(
        "C5", seed, samples, SAMPLE_COUNT, &config, EVENT_INDEX, 24u
    ));
    printf("NOTE C5 synthetic-only caller-configured 50Hz notch and 8-180Hz bandpass; not deployed acceptance\n");
}


static void test_c6_isolated_impulse_outlier_known_defect(void)
{
    const uint32_t seed = 0x13200606u;
    const size_t outlier_index = 400u;
    const size_t physical_event_index = 900u;
    float samples[SAMPLE_COUNT];
    fill_white_noise(samples, SAMPLE_COUNT, seed, 0.01f);
    samples[outlier_index] += 0.8f;
    add_ramp_pulse(samples, SAMPLE_COUNT, physical_event_index, 40u, 0.9f);
    check_finite_samples("C6", seed, samples, SAMPLE_COUNT);

    geophone_channel_pick_config_t config = pick_config();
    config.noise_window_samples = 128u; /* the seeded background before the outlier */
    geophone_channel_pick_t pick;
    const bool accepted = geophone_event_pick_channel_arrival(
        samples, SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &pick
    );
    check_finite_pick("C6", seed, &pick);
    CHECK("C6", seed, accepted);
    CHECK("C6", seed, pick.status == GEOPHONE_PICK_ACCEPTED);
    CHECK("C6", seed, fabs(sample_position(&pick) - (double)outlier_index) <= 1.0);
    const double source_error = sample_position(&pick) - (double)physical_event_index;
    printf("CASE C6 detection=%s pick_error_vs_physical_event_samples=%.3f\n",
           status_name(pick.status), source_error);
    printf("KNOWN_DEFECT C6 isolated impulsive outlier is accepted before the later source event\n");
}


static void test_c7_clipped_waveform(void)
{
    const uint32_t seed = 0x13200707u;
    float samples[SAMPLE_COUNT];
    fill_white_noise(samples, SAMPLE_COUNT, seed, 0.004f);
    add_ramp_pulse(samples, SAMPLE_COUNT, EVENT_INDEX, 40u, 2.0f);
    bool clipped = false;
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        if (samples[i] > 0.35f) { samples[i] = 0.35f; clipped = true; }
        if (samples[i] < -0.35f) { samples[i] = -0.35f; clipped = true; }
    }
    CHECK("C7", seed, clipped);
    check_finite_samples("C7", seed, samples, SAMPLE_COUNT);
    const geophone_channel_pick_config_t config = pick_config();
    report_error("C7", check_event_pick(
        "C7", seed, samples, SAMPLE_COUNT, &config, EVENT_INDEX, EVENT_TOLERANCE_SAMPLES
    ));
}


static int16_t saturate_i16(float value)
{
    if (value >= 32767.0f) return INT16_MAX;
    if (value <= -32768.0f) return INT16_MIN;
    return (int16_t)lrintf(value);
}


static void test_c8_ad7606_int16_saturation_known_defect(void)
{
    const uint32_t seed = 0x13200808u;
    float samples[SAMPLE_COUNT];
    fill_white_noise(samples, SAMPLE_COUNT, seed, 20.0f);
    add_ramp_pulse(samples, SAMPLE_COUNT, EVENT_INDEX, 40u, 60000.0f);
    check_finite_samples("C8", seed, samples, SAMPLE_COUNT);
    size_t saturated_samples = 0u;
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        const int16_t raw = saturate_i16(samples[i]);
        if (raw == INT16_MAX || raw == INT16_MIN) ++saturated_samples;
        samples[i] = (float)raw;
    }
    CHECK("C8", seed, saturated_samples > 0u);
    const geophone_channel_pick_config_t config = pick_config();
    const double error = check_event_pick(
        "C8", seed, samples, SAMPLE_COUNT, &config, EVENT_INDEX, EVENT_TOLERANCE_SAMPLES
    );
    printf("CASE C8 detection=ACCEPTED saturated_samples=%zu max_pick_error_samples=%.3f\n",
           saturated_samples, error);
    printf("KNOWN_DEFECT C8 raw int16 saturation is accepted without a saturation rejection status\n");
}


static double check_cross_channel_errors(
    const char *name,
    uint32_t seed,
    const geophone_channel_pick_t *picks,
    const size_t *truth,
    const bool *present,
    size_t channel_count,
    size_t tolerance
)
{
    double max_error = 0.0;
    for (size_t ch = 0u; ch < channel_count; ++ch) {
        if (!present[ch]) continue;
        const double error = fabs(sample_position(&picks[ch]) - (double)truth[ch]);
        CHECK(name, seed, error <= (double)tolerance);
        if (error > max_error) max_error = error;
    }
    for (size_t a = 0u; a < channel_count; ++a) {
        if (!present[a]) continue;
        for (size_t b = a + 1u; b < channel_count; ++b) {
            if (!present[b]) continue;
            const double measured = sample_position(&picks[b]) - sample_position(&picks[a]);
            const double expected = (double)truth[b] - (double)truth[a];
            CHECK(name, seed, fabs(measured - expected) <= 2.0 * (double)tolerance);
        }
    }
    return max_error;
}


static void test_c9_missing_channel_is_dropped(void)
{
    const uint32_t seed = 0x13200909u;
    static const size_t delays[4] = { 0u, 4u, 8u, 12u };
    float traces[4][SAMPLE_COUNT] = { { 0.0f } };
    geophone_channel_pick_t picks[4];
    bool present[4] = { true, true, false, true };
    size_t truth[4];
    const geophone_channel_pick_config_t config = pick_config();

    for (size_t ch = 0u; ch < 4u; ++ch) {
        truth[ch] = EVENT_INDEX + delays[ch];
        if (present[ch]) {
            fill_white_noise(traces[ch], SAMPLE_COUNT, seed + (uint32_t)ch, 0.004f);
            add_ramp_pulse(traces[ch], SAMPLE_COUNT, truth[ch], 40u, 0.8f);
        }
        check_finite_samples("C9", seed, traces[ch], SAMPLE_COUNT);
        const bool accepted = geophone_event_pick_channel_arrival(
            traces[ch], SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &picks[ch]
        );
        check_finite_pick("C9", seed, &picks[ch]);
        CHECK("C9", seed, accepted == present[ch]);
        if (present[ch]) CHECK("C9", seed, picks[ch].status == GEOPHONE_PICK_ACCEPTED);
        else CHECK("C9", seed, picks[ch].status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
    }
    const double max_error = check_cross_channel_errors(
        "C9", seed, picks, truth, present, 4u, EVENT_TOLERANCE_SAMPLES
    );
    CHECK("C9", seed, !geophone_event_cross_correlate(
        traces[0], traces[2], SAMPLE_COUNT,
        &(geophone_cross_correlation_config_t) { .max_lag_samples = 24u },
        &(geophone_cross_correlation_result_t) { 0 }
    ));
    printf("CASE C9 detection=3/4 accepted missing_channel=REJECTED_NO_TRIGGER max_pick_error_samples=%.3f\n",
           max_error);
}


static void build_delayed_duplicate_traces(
    float traces[4][SAMPLE_COUNT],
    const size_t delays[4],
    uint32_t seed,
    float primary_amplitude,
    float echo_amplitude,
    size_t echo_delay
)
{
    for (size_t ch = 0u; ch < 4u; ++ch) {
        fill_white_noise(traces[ch], SAMPLE_COUNT, seed + (uint32_t)ch, 0.003f);
        add_ramp_pulse(traces[ch], SAMPLE_COUNT, EVENT_INDEX + delays[ch], 32u, primary_amplitude);
        add_ramp_pulse(traces[ch], SAMPLE_COUNT, EVENT_INDEX + delays[ch] + echo_delay, 32u, echo_amplitude);
        check_finite_samples("multi-channel", seed, traces[ch], SAMPLE_COUNT);
    }
}


static void assert_known_delay(
    const char *name,
    uint32_t seed,
    const float *a,
    const float *b,
    size_t expected_delay
)
{
    geophone_cross_correlation_result_t result;
    const geophone_cross_correlation_config_t config = { .max_lag_samples = 24u };
    CHECK(name, seed, geophone_event_cross_correlate(a, b, SAMPLE_COUNT, &config, &result));
    CHECK(name, seed, isfinite(result.peak_correlation));
    CHECK(name, seed, isfinite(result.delay_refined_samples));
    CHECK(name, seed, fabs(result.delay_refined_samples - (double)expected_delay) <= 2.0);
}


static void test_c10_delayed_duplicate_event_and_cross_channel_delay(void)
{
    const uint32_t seed = 0x13201010u;
    static const size_t delays[4] = { 0u, 5u, 10u, 15u };
    float traces[4][SAMPLE_COUNT];
    geophone_channel_pick_t picks[4];
    bool present[4] = { true, true, true, true };
    size_t truth[4];
    const geophone_channel_pick_config_t config = pick_config();
    build_delayed_duplicate_traces(traces, delays, seed, 0.8f, 0.8f, 180u);
    for (size_t ch = 0u; ch < 4u; ++ch) {
        truth[ch] = EVENT_INDEX + delays[ch];
        CHECK("C10", seed, geophone_event_pick_channel_arrival(
            traces[ch], SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &picks[ch]
        ));
        check_finite_pick("C10", seed, &picks[ch]);
        CHECK("C10", seed, picks[ch].status == GEOPHONE_PICK_ACCEPTED);
    }
    const double max_error = check_cross_channel_errors(
        "C10", seed, picks, truth, present, 4u, EVENT_TOLERANCE_SAMPLES
    );
    assert_known_delay("C10", seed, traces[0], traces[1], delays[1]);
    printf("CASE C10 detection=4/4 accepted cross_channel_delay=5 max_pick_error_samples=%.3f\n",
           max_error);
}


static void test_c11_multipath_prefers_first_arrival(void)
{
    const uint32_t seed = 0x13201111u;
    static const size_t delays[4] = { 0u, 3u, 6u, 9u };
    float traces[4][SAMPLE_COUNT];
    geophone_channel_pick_t picks[4];
    bool present[4] = { true, true, true, true };
    size_t truth[4];
    const geophone_channel_pick_config_t config = pick_config();
    build_delayed_duplicate_traces(traces, delays, seed, 0.20f, 1.2f, 80u);
    for (size_t ch = 0u; ch < 4u; ++ch) {
        truth[ch] = EVENT_INDEX + delays[ch];
        CHECK("C11", seed, geophone_event_pick_channel_arrival(
            traces[ch], SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &picks[ch]
        ));
        check_finite_pick("C11", seed, &picks[ch]);
        CHECK("C11", seed, picks[ch].status == GEOPHONE_PICK_ACCEPTED);
    }
    const double max_error = check_cross_channel_errors(
        "C11", seed, picks, truth, present, 4u, EVENT_TOLERANCE_SAMPLES
    );
    assert_known_delay("C11", seed, traces[0], traces[1], delays[1]);
    printf("CASE C11 detection=4/4 accepted first_arrival_before_larger_echo max_pick_error_samples=%.3f\n",
           max_error);
}


static void synth_p_s_trace(
    float *samples,
    size_t count,
    uint32_t seed,
    size_t p_index,
    size_t s_index
)
{
    uint32_t state = seed;
    for (size_t i = 0u; i < count; ++i) {
        const float noise = 0.006f * rng_signed(&state);
        float value = noise;
        if (i >= p_index) {
            const float t = (float)(i - p_index);
            value += expf(-t / 60.0f) * sinf(2.0f * PI_F * 0.08f * t);
        }
        if (i >= s_index) {
            const float t = (float)(i - s_index);
            value += 3.0f * expf(-t / 90.0f) * sinf(2.0f * PI_F * 0.03f * t);
        }
        samples[i] = value;
    }
}


static void test_c12_known_p_s_separation(void)
{
    const uint32_t seed = 0x13201212u;
    enum { PS_COUNT = 1200u, P_INDEX = 240u, S_DELTA = 160u };
    geophone_channel_pick_config_t p_config = pick_config();
    p_config.noise_window_samples = 100u; /* pre-event noise only; P begins at sample 240. */
    const geophone_secondary_pick_config_t s_config = {
        .min_separation_samples = 20u,
        .max_separation_samples = 320u,
        .sta_window_samples = 10u,
        .lta_window_samples = 60u,
        .sta_lta_trigger_ratio = 3.0f,
        .minimum_snr = 2.0f
    };
    double max_p_error = 0.0;
    double max_s_error = 0.0;

    for (size_t ch = 0u; ch < 4u; ++ch) {
        const size_t delay = ch * 3u;
        const size_t p_truth = P_INDEX + delay;
        const size_t s_truth = p_truth + S_DELTA;
        const uint32_t trace_seed = seed + (uint32_t)ch;
        float trace[PS_COUNT];
        synth_p_s_trace(trace, PS_COUNT, trace_seed, p_truth, s_truth);
        check_finite_samples("C12", trace_seed, trace, PS_COUNT);
        geophone_channel_pick_t p_pick;
        CHECK("C12", trace_seed, geophone_event_pick_channel_arrival(
            trace, PS_COUNT, SAMPLE_RATE_HZ, &p_config, &p_pick
        ));
        check_finite_pick("C12", trace_seed, &p_pick);
        CHECK("C12", trace_seed, p_pick.status == GEOPHONE_PICK_ACCEPTED);
        const double p_error = fabs(sample_position(&p_pick) - (double)p_truth);
        CHECK("C12", trace_seed, p_error <= 20.0); /* 80 Hz P period is 12.5 samples; plus rise/search quantization. */
        if (p_error > max_p_error) max_p_error = p_error;

        geophone_channel_pick_t s_pick;
        CHECK("C12", trace_seed, geophone_event_pick_secondary_arrival(
            trace, PS_COUNT, SAMPLE_RATE_HZ, p_pick.arrival_sample_index, &s_config, &s_pick
        ));
        check_finite_pick("C12-S", trace_seed, &s_pick);
        CHECK("C12", trace_seed, s_pick.status == GEOPHONE_PICK_ACCEPTED);
        const double s_error = fabs(sample_position(&s_pick) - (double)s_truth);
        CHECK("C12", trace_seed, s_error <= 20.0);
        CHECK("C12", trace_seed, fabs((sample_position(&s_pick) - sample_position(&p_pick)) - (double)S_DELTA) <= 24.0);
        if (s_error > max_s_error) max_s_error = s_error;
    }
    printf("CASE C12 detection=P_and_S_4/4 known_delta_samples=%u max_P_error_samples=%.3f max_S_error_samples=%.3f\n",
           (unsigned)S_DELTA, max_p_error, max_s_error);
}


static void test_c13_two_events_first_only_characterization(void)
{
    const uint32_t seed = 0x13201313u;
    static const size_t delays[4] = { 0u, 3u, 6u, 9u };
    const size_t second_index = 1120u;
    float traces[4][SAMPLE_COUNT] = { { 0.0f } };
    geophone_channel_pick_t picks[4];
    bool present[4] = { true, true, true, true };
    size_t truth[4];
    const geophone_channel_pick_config_t config = pick_config();

    for (size_t ch = 0u; ch < 4u; ++ch) {
        truth[ch] = EVENT_INDEX + delays[ch];
        add_ramp_pulse(traces[ch], SAMPLE_COUNT, truth[ch], 32u, 0.45f);
        add_ramp_pulse(traces[ch], SAMPLE_COUNT, second_index + delays[ch], 32u, 1.2f);
        check_finite_samples("C13", seed, traces[ch], SAMPLE_COUNT);
        CHECK("C13", seed, geophone_event_pick_channel_arrival(
            traces[ch], SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &picks[ch]
        ));
        check_finite_pick("C13", seed, &picks[ch]);
        CHECK("C13", seed, picks[ch].status == GEOPHONE_PICK_ACCEPTED);
        CHECK("C13", seed, sample_position(&picks[ch]) < (double)second_index);
    }
    const double max_error = check_cross_channel_errors(
        "C13", seed, picks, truth, present, 4u, EVENT_TOLERANCE_SAMPLES
    );
    printf("CASE C13 detection=first_event_only both_events_present=yes max_pick_error_samples=%.3f\n",
           max_error);
    printf("KNOWN_DEFECT C13 second event is not surfaced as ambiguity or a second pick; safe_pipeline_match=NO\n");
}


static void add_random_phase_burst(
    float *samples,
    size_t count,
    size_t onset,
    float amplitude,
    float phase_radians
)
{
    const size_t burst_samples = 80u;
    const size_t rise_samples = 8u;
    for (size_t k = 0u; k < burst_samples && onset + k < count; ++k) {
        const float envelope = k < rise_samples
            ? (float)(k + 1u) / (float)rise_samples
            : expf(-(float)(k - rise_samples) / 90.0f);
        const float phase = 2.0f * PI_F * 80.0f * (float)k / (float)SAMPLE_RATE_HZ + phase_radians;
        samples[onset + k] += amplitude * envelope * sinf(phase);
    }
}


static void test_c14_random_phase_and_amplitude_channels(void)
{
    static const uint32_t seeds[] = {
        0xC1400001u, 0xC1400002u, 0xC1400003u, 0xC1400004u,
        0xC1400005u, 0xC1400006u, 0xC1400007u, 0xC1400008u
    };
    const size_t tolerance = 22u; /* ceil(1 kHz / 80 Hz) + eight-sample envelope rise. */
    geophone_channel_pick_config_t config = pick_config();
    config.refine_lookback_samples = 32u;
    config.refine_lookahead_samples = 64u;
    config.snr_signal_window_samples = 64u;
    size_t false_negatives = 0u;
    double max_error = 0.0;
    double max_channel_spread = 0.0;

    for (size_t trial = 0u; trial < sizeof(seeds) / sizeof(seeds[0]); ++trial) {
        uint32_t state = seeds[trial];
        double earliest = 1.0e9;
        double latest = -1.0e9;
        for (size_t ch = 0u; ch < 4u; ++ch) {
            float trace[SAMPLE_COUNT];
            fill_white_noise(trace, SAMPLE_COUNT, rng_next(&state), 0.008f);
            const float amplitude = 0.35f + 0.65f * ((rng_next(&state) >> 8) / 16777216.0f);
            const float phase = 2.0f * PI_F * ((rng_next(&state) >> 8) / 16777216.0f);
            add_random_phase_burst(trace, SAMPLE_COUNT, EVENT_INDEX, amplitude, phase);
            check_finite_samples("C14", seeds[trial], trace, SAMPLE_COUNT);
            geophone_channel_pick_t pick;
            const bool accepted = geophone_event_pick_channel_arrival(
                trace, SAMPLE_COUNT, SAMPLE_RATE_HZ, &config, &pick
            );
            check_finite_pick("C14", seeds[trial], &pick);
            if (!accepted) ++false_negatives;
            CHECK("C14", seeds[trial], accepted);
            CHECK("C14", seeds[trial], pick.status == GEOPHONE_PICK_ACCEPTED);
            const double position = sample_position(&pick);
            const double error = fabs(position - (double)EVENT_INDEX);
            CHECK("C14", seeds[trial], error <= (double)tolerance);
            if (error > max_error) max_error = error;
            if (position < earliest) earliest = position;
            if (position > latest) latest = position;
        }
        const double spread = latest - earliest;
        CHECK("C14", seeds[trial], spread <= 2.0 * (double)tolerance);
        if (spread > max_channel_spread) max_channel_spread = spread;
    }
    CHECK("C14", seeds[0], false_negatives == 0u);
    printf("CASE C14 detection=32/32 accepted max_pick_error_samples=%.3f max_channel_spread_samples=%.3f false_negatives=%zu/32\n",
           max_error, max_channel_spread, false_negatives);
}


int main(void)
{
    test_c1_all_zeros();
    test_c2_constant_dc();
    test_c3_white_noise_snr_sweep();
    test_c4_colored_drift();
    test_c5_configured_50hz_notch_and_bandpass();
    test_c6_isolated_impulse_outlier_known_defect();
    test_c7_clipped_waveform();
    test_c8_ad7606_int16_saturation_known_defect();
    test_c9_missing_channel_is_dropped();
    test_c10_delayed_duplicate_event_and_cross_channel_delay();
    test_c11_multipath_prefers_first_arrival();
    test_c12_known_p_s_separation();
    test_c13_two_events_first_only_characterization();
    test_c14_random_phase_and_amplitude_channels();
    return EXIT_SUCCESS;
}
